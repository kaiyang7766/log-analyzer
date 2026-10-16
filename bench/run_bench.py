#!/usr/bin/env python3
"""End-to-end extraction benchmark for the log-analyzer CLI.

Each configuration (file x io mode) is run WARMUP times untimed, then RUNS
times timed. Timed runs are interleaved: every iteration executes each
configuration once in shuffled order, so drift (thermal, background load)
spreads evenly across configurations instead of biasing one of them.

Per run, os.wait4 gives wall time plus the child's own user/sys CPU and peak
RSS. stdout goes to /dev/null so terminal rendering is not measured. Every run
must exit 0, and output is checked once for determinism before timing.

Usage:
  python3 bench/run_bench.py [--runs 50] [--warmup 5] [--binary PATH]
                             [--json OUT] FILE[:input-format] ...
"""

import argparse
import hashlib
import json
import os
import platform
import random
import statistics
import subprocess
import sys
import time


def percentile(sorted_values, p):
    """Linear interpolation between closest ranks (numpy's default method)."""
    if not sorted_values:
        return float("nan")
    k = (len(sorted_values) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(sorted_values) - 1)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (k - lo)


def run_once(argv):
    devnull = os.open(os.devnull, os.O_WRONLY)
    try:
        start = time.perf_counter_ns()
        proc = subprocess.Popen(argv, stdout=devnull, stderr=subprocess.PIPE)
        _, status, usage = os.wait4(proc.pid, 0)
        wall_ns = time.perf_counter_ns() - start
        proc.returncode = os.waitstatus_to_exitcode(status) if hasattr(os, "waitstatus_to_exitcode") else (status >> 8)
        stderr = proc.stderr.read()
        proc.stderr.close()
    finally:
        os.close(devnull)
    if proc.returncode != 0:
        raise RuntimeError(f"{' '.join(argv)} exited {proc.returncode}: {stderr.decode(errors='replace').strip()}")
    # ru_maxrss is bytes on macOS, kilobytes on Linux.
    rss = usage.ru_maxrss if sys.platform == "darwin" else usage.ru_maxrss * 1024
    return {
        "wall_ms": wall_ns / 1e6,
        "user_ms": usage.ru_utime * 1e3,
        "sys_ms": usage.ru_stime * 1e3,
        "rss_mb": rss / (1024 * 1024),
    }


def summarize(values):
    s = sorted(values)
    mean = statistics.fmean(s)
    stdev = statistics.stdev(s) if len(s) > 1 else 0.0
    return {
        "min": s[0], "p50": percentile(s, 50), "p95": percentile(s, 95),
        "p99": percentile(s, 99), "max": s[-1], "mean": mean, "stdev": stdev,
        "cv_pct": (stdev / mean * 100) if mean else 0.0,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", nargs="+", help="log file, optionally suffixed with :input-format")
    parser.add_argument("--binary", default="build-release/log-analyzer")
    parser.add_argument("--runs", type=int, default=50)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--io", default="buffered,mmap", help="comma-separated io modes")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--json", help="write raw samples and summaries here")
    args = parser.parse_args()

    configs = []
    for spec in args.files:
        path, _, fmt = spec.partition(":")
        for io in args.io.split(","):
            argv = [args.binary, "analyze", path, "--io", io]
            if fmt:
                argv += ["--input-format", fmt]
            configs.append({
                "name": f"{os.path.basename(path)} [{io}]", "file": path, "io": io,
                "input_format": fmt or "auto", "size_bytes": os.path.getsize(path),
                "argv": argv, "samples": [],
            })
    baseline = {"name": "spawn baseline (/usr/bin/true)", "argv": ["/usr/bin/true"], "samples": []}

    # Determinism: identical output across repeated runs of each configuration.
    for c in configs:
        digests = {hashlib.sha256(subprocess.run(c["argv"], capture_output=True, check=True).stdout).hexdigest()
                   for _ in range(3)}
        if len(digests) != 1:
            sys.exit(f"non-deterministic output for {c['name']}")
        c["output_sha256"] = digests.pop()

    print(f"binary: {args.binary}\nruns: {args.runs} timed + {args.warmup} warm-up per config, interleaved\n")
    for c in configs + [baseline]:
        for _ in range(args.warmup):
            run_once(c["argv"])

    rng = random.Random(args.seed)
    order = configs + [baseline]
    for i in range(args.runs):
        rng.shuffle(order)
        for c in order:
            c["samples"].append(run_once(c["argv"]))
        print(f"\riteration {i + 1}/{args.runs}", end="", flush=True)
    print("\n")

    for c in configs + [baseline]:
        c["summary"] = {k: summarize([s[k] for s in c["samples"]]) for k in ("wall_ms", "user_ms", "sys_ms", "rss_mb")}
        if "size_bytes" in c:
            mb = c["size_bytes"] / 1e6
            c["summary"]["throughput_mb_s_at_p50"] = mb / (c["summary"]["wall_ms"]["p50"] / 1e3)

    header = f"{'config':<48} {'size':>9} {'p50 ms':>9} {'p95 ms':>9} {'p99 ms':>9} {'min':>8} {'max':>8} {'CV%':>5} {'cpu p50':>8} {'RSS MB':>7} {'MB/s':>7}"
    print(header)
    print("-" * len(header))
    for c in configs + [baseline]:
        w = c["summary"]["wall_ms"]
        cpu = statistics.median(s["user_ms"] + s["sys_ms"] for s in c["samples"])
        rss = c["summary"]["rss_mb"]["p50"]
        size = f"{c['size_bytes'] / 1e6:.2f}MB" if "size_bytes" in c else "-"
        tput = f"{c['summary']['throughput_mb_s_at_p50']:.0f}" if "size_bytes" in c else "-"
        print(f"{c['name']:<48} {size:>9} {w['p50']:>9.2f} {w['p95']:>9.2f} {w['p99']:>9.2f} {w['min']:>8.2f} "
              f"{w['max']:>8.2f} {w['cv_pct']:>5.1f} {cpu:>8.1f} {rss:>7.1f} {tput:>7}")

    if args.json:
        meta = {
            "binary": args.binary, "runs": args.runs, "warmup": args.warmup, "seed": args.seed,
            "machine": platform.platform(), "processor": platform.processor(),
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        }
        with open(args.json, "w") as fh:
            json.dump({"meta": meta, "configs": configs + [baseline]}, fh, indent=2)
        print(f"\nraw samples: {args.json}")


if __name__ == "__main__":
    main()
