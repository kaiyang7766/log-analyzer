# log-analyzer

log-analyzer is a fast, single-threaded C++20 command-line tool. It reduces a
large raw log file to a compact incident summary, so you can give that summary
to an LLM instead of the whole log. Each invocation analyzes one file.

**Input:** one log file. The format is detected automatically: JSONL,
application exports, Logback, Spark/YARN syslog (including browser-saved HTML
`<xmp>` pages), Flink JobManager logs (including NUL-padded ones), and
scheduler instance logs. If detection is ambiguous, use `--input-format`.
Multiline stack traces are attached to the event that logged them.

**Output:** a text, Markdown, or JSON summary. It covers the time range, the
resolved outcome (succeeded, failed, or unknown) with supporting evidence,
event counts, failure, exception, and warning groups, Spark/Flink lifecycle
totals, retries, and latency percentiles. The `errors` and `latency` views and
filters (`--from`, `--to`, `--level`, `--service`, `--trace-id`, `--text`,
`--limit`) narrow the summary for follow-up questions.

## Build and run

Requirements: CMake 3.20+, Ninja, a C++20 compiler, and simdjson.

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/log-analyzer analyze jobmanager.log --io mmap --format markdown
```

## Performance

Measured on an Apple Silicon Mac (macOS 26.3, arm64) with a Release build.
Each configuration got 5 warm-up runs and 50 timed runs, interleaved in
shuffled order. Times are end-to-end process wall time with stdout sent to
`/dev/null`. Output was checked to be deterministic before timing.

| Log | Size | I/O | p50 | p95 | CPU p50 | Peak RSS | Throughput |
|---|---:|---|---:|---:|---:|---:|---:|
| jobmanager.log (Flink) | 95.8 MB | mmap | **95.3 ms** | 97.5 ms | 94.5 ms | 94 MB | **1006 MB/s** |
| jobmanager.log (Flink) | 95.8 MB | buffered | 111.8 ms | 126.4 ms | 110.0 ms | 94 MB | 857 MB/s |
| spark_submit_a.log | 1.03 MB | mmap | 7.06 ms | 7.57 ms | 6.3 ms | 4.5 MB | 147 MB/s |
| spark_submit_a.log | 1.03 MB | buffered | 7.31 ms | 7.79 ms | 6.5 ms | 4.8 MB | 141 MB/s |
| spark_submit_b.log (spark-syslog) | 17 KB | mmap | 2.12 ms | 2.68 ms | 1.4 ms | 1.5 MB | — |
| spark_submit_b.log (spark-syslog) | 17 KB | buffered | 2.18 ms | 2.64 ms | 1.5 ms | 1.5 MB | — |
| `/usr/bin/true` (process spawn baseline) | — | — | 1.53 ms | 2.23 ms | 0.9 ms | 1.1 MB | — |

- On large files, `--io mmap` is about 15% faster at p50 and much steadier at
  p95. Most of the gain is lower system CPU time (4.0 ms vs 15.5 ms), because
  mmap skips the whole-file copy.
- At about 1 GB/s, nearly all large-file time is spent parsing in user space.
- On small files, process startup dominates. The 17 KB log takes about 0.6 ms
  more than an empty process.
- Peak RSS is about equal to the file size on large inputs, in both I/O modes.

Raw samples: [`bench/results/isolated_logs_20261011_230330.json`](bench/results/isolated_logs_20261011_230330.json)
(2026-10-11). To reproduce:

```bash
python3 bench/run_bench.py --runs 50 --warmup 5 \
  --json bench/results/<name>.json \
  isolated_logs/spark_submit_b.log:spark-syslog \
  isolated_logs/spark_submit_a.log \
  isolated_logs/jobmanager.log
```
