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

Measured on an Apple Silicon Mac (macOS 26.3, arm64) with a Release build,
on 2026-10-25. Each configuration got 5 warm-up runs and 50 timed runs,
interleaved in shuffled order. Times are end-to-end process wall time with
stdout sent to `/dev/null`. Output was checked to be deterministic before
timing.

| Log | Size | I/O | p50 | p95 | CPU p50 | Peak RSS | Throughput |
|---|---:|---|---:|---:|---:|---:|---:|
| jobmanager.log (Flink) | 95.8 MB | mmap | **51.0 ms** | 52.6 ms | 50.1 ms | 94 MB | **1880 MB/s** |
| jobmanager.log (Flink) | 95.8 MB | buffered | 69.0 ms | 89.4 ms | 66.8 ms | 94 MB | 1390 MB/s |
| spark_submit_a.log (Spark) | 1.03 MB | mmap | 6.56 ms | 7.22 ms | 5.7 ms | 4.5 MB | 158 MB/s |
| spark_submit_a.log (Spark) | 1.03 MB | buffered | 6.65 ms | 7.37 ms | 5.8 ms | 4.6 MB | 155 MB/s |
| spark_submit_b.log (Spark) | 17 KB | mmap | 2.17 ms | 3.09 ms | 1.5 ms | 1.5 MB | — |
| spark_submit_b.log (Spark) | 17 KB | buffered | 2.32 ms | 3.84 ms | 1.6 ms | 1.5 MB | — |
| `/usr/bin/true` (process spawn baseline) | — | — | 1.56 ms | 1.95 ms | 1.0 ms | 1.1 MB | — |

- 56% of `jobmanager.log` (53.4 MB) is leading NUL padding from a
  preallocated file. Throughput counts the whole file.
- On large files, `--io mmap` is about 26% faster than buffered. System CPU
  drops from 15.8 ms to 4.0 ms because mmap skips the whole-file copy.
- On small files, process startup dominates. The 17 KB log takes about 0.6 ms
  more than an empty process.
- Peak RSS is about equal to the file size on large inputs, in both I/O modes.

Raw samples: [`bench/results/string_opt_progression_20261025.json`](bench/results/string_opt_progression_20261025.json).
To reproduce (pass `--binary NAME=PATH` more than once to compare builds in
one interleaved session):

```bash
python3 bench/run_bench.py --runs 50 --warmup 5 \
  --json bench/results/<name>.json \
  isolated_logs/spark_submit_b.log:spark-syslog \
  isolated_logs/spark_submit_a.log \
  isolated_logs/jobmanager.log
```

## Optimization log

Every step keeps the output byte-for-byte identical: each was checked against
saved text, Markdown, JSON, `errors` and `latency` output for the benchmark
logs and the test fixtures. Each step was measured against the previous build
in the same interleaved session, because two separate sessions on this
machine can differ by several milliseconds on the same binary.

### Round 1: string scanning (2026-10-25)

p50 wall time with `--io mmap`. All four builds were run in one session.

| Step | jobmanager.log | spark_submit_a.log |
|---|---:|---:|
| Baseline | 70.4 ms | 7.08 ms |
| 1. Probe the container instead of scanning it | 60.7 ms (−14%) | 7.24 ms (±noise) |
| 2. Parse each header line once | 52.2 ms (−14%) | 7.02 ms (−3%) |
| 3. Cheaper error normalization | 51.0 ms (−2%) | 6.56 ms (−7%) |
| **Total** | **−28%** (1360 → 1880 MB/s) | **−7%** |

With buffered I/O, jobmanager.log went from 88.6 ms to 69.0 ms (−22%).
The 17 KB log did not change because process startup dominates it.

The baseline here is 70 ms rather than the 95 ms published on 2026-10-11.
That earlier run was measured on an older build.

**1. Probe the container instead of scanning it.** Before parsing, the input
is checked for a browser-saved HTML wrapper (`<xmp>`) and for leading NUL
padding.
- *Before:* `find("<xmp")` read the entire 96 MB file even though plain logs
  never contain it. Then `find_first_not_of('\0')` walked the 53 MB of padding
  one byte per loop iteration.
- *After:* the padding is skipped 64 bytes per step, by OR-ing eight 8-byte
  words and testing for zero. The `<xmp` search runs only when the first
  visible byte is `<`, because a saved page starts with markup and a log starts
  with a record.
- *Gain:* about 10 ms on jobmanager.log.
- *Correctness:* this also fixes a bug. A plain log that mentioned `<xmp`
  in a message used to be treated as HTML.
- *Lesson:* the first version used `find_first_not_of` with a set of
  characters to skip whitespace and NULs together. That made the run 15%
  slower, because the set version checks every byte against the set. Two
  surprises came from profiling, not from reading the code: that slowdown, and
  the fact that the padding is 56% of the file.

**2. Parse each header line once.** Framing decides where each event starts
by testing every line with the format's header parser.
- *Before:* the parser ran once with no output to decide whether a line was a
  header. It ran a second time on the same line to fill in the record. Scheduler
  logs tried up to four parsers per line, then ran the winning one again.
- *After:* the scan returns the header fields as `string_view`s into the
  input: timestamp, level, logger, thread and message. Framing hands those to
  the decoder, which only copies them into the record.
- *Gain:* about 8.5 ms on jobmanager.log. Its 94k events are mostly
  single-line, so header parsing was a large share of the work. The
  `"[] - "` search and the token splitting no longer run twice.

**3. Cheaper error normalization.** `normalize_error` builds the grouping key
for every warning, error and exception. It lowercases the text and replaces
UUIDs, hex addresses, IPs and numbers with placeholders.
- *Before:*
  - All four pattern matchers ran at every character, though a pattern can
    only start at a word boundary on a digit or `a`–`f`.
  - Lowercasing called libc `std::tolower` once per character, through a
    dynamic-library stub and a locale check.
  - The UUID matcher searched a list of hyphen positions for each character it
    compared.
- *After:*
  - The matchers run only where a pattern can start. Every other word is copied
    whole in one step.
  - Lowercasing and character tests use inline ASCII comparisons. The program
    never sets a locale, so these match the "C" locale results exactly.
- *Verification:* a differential test compared the old and new versions on
  1,000,000 random strings and found no difference.
- *Gain:* the gain scales with the share of warning and error lines.

  | Log | Warning/error/exception events | Change |
  |---|---:|---:|
  | jobmanager.log | 1.7% | −2% |
  | spark_submit_a.log | 63% | −7% |
  | Synthetic warning-heavy 100 MB Flink log | 67% | −19% |

### Next candidates

Measured costs that remain:
- Each record allocates about 12 `std::string` fields and an attribute map.
  `consume_record` copies `service` and `endpoint` and builds temporary map
  keys on every record.
- `lower()` and `contains_case_insensitive` allocate lowercase copies for
  every severity check and keyword test.
- `std::isspace` is still called through libc in `take_token` and
  `trim_view`.
- `number_after` parses with `std::stod` on a temporary string.
