# LogScope++

LogScope++ is a fast, local C++20 command-line log pre-analyzer designed
primarily for single-threaded execution. Its main purpose is to reduce large
collections of raw logs into compact, structured incident summaries before
those summaries are provided to an LLM for debugging.

Instead of spending LLM context and upload time on entire log files, LogScope++
scans each file locally, reconstructs multiline records, extracts workflow,
Spark, and Flink lifecycle signals, groups failures, and produces text,
Markdown, or JSON summaries. It is designed for high-throughput batch workflows
where many large files are analyzed independently, with one file processed per
invocation.

The CLI can be called directly by a developer or exposed as a local tool to an
LLM harness with shell-command capabilities. The harness can run an initial
summary, inspect the compact result, and issue focused follow-up commands with
filters or the `errors` and `latency` views without sending the raw log to the
model.

## Supported Inputs

Auto detection currently recognizes:

- JSONL objects using the built-in field mapping
- enriched application exports with an outer metadata envelope and `_msg=`
- raw Logback-style application logs
- raw Spark/YARN syslog
- Flink JobManager console logs such as `jobmanager.out`
- NUL-padded Flink `jobmanager.log` files with full-year, column-aligned timestamps
- browser-saved Spark syslog wrapped in an HTML `<xmp>` element

Use `--input-format` when automatic detection is ambiguous:

```bash
--input-format auto|jsonl|app-export|logback|spark-syslog|flink-console
```

Multiline stack traces and diagnostic blocks are attached to the preceding
event. Blank lines are not treated as record boundaries. Leading NUL padding is
skipped through a view into the original input rather than by copying the
remaining payload.

## Build

Requirements: CMake 3.20+, Ninja, a C++20 compiler, and simdjson.

```bash
# macOS
brew install cmake ninja simdjson

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On Debian/Ubuntu, install `cmake`, `ninja-build`, `g++`, and
`libsimdjson-dev`.

## Run

```bash
./build/logscope analyze incident.txt
./build/logscope analyze spark.syslog.html --timezone +08:00
./build/logscope analyze jobmanager.out --timezone +08:00
./build/logscope errors incident.txt --group-by service
./build/logscope latency spark.syslog.html --group-by endpoint
./build/logscope analyze incident.txt --text timeout --format markdown
./build/logscope analyze incident.txt --format json
./build/logscope analyze incident.txt --io mmap
```

For large logs, use a Release build and consider `--io mmap` to avoid copying
the input file into a userspace buffer.

To pre-analyze a directory in bulk, invoke LogScope++ once per file and collect
the compact summaries for an LLM harness:

```bash
find /path/to/logs -type f \
  -exec ./build/logscope analyze {} --format markdown --io mmap \; \
  > bulk-summary.md
```

This keeps the analyzer itself deterministic and single-threaded while allowing
the surrounding batch workflow to control file selection, output organization,
and concurrency.

Common filters and output controls:

```text
--from TIMESTAMP
--to TIMESTAMP
--level LEVEL
--service SERVICE
--trace-id ID
--text TEXT
--limit COUNT
```

`--limit` caps grouped output rows; it does not change which records are
analyzed.

Spark and Flink console timestamps have no timezone in the source format.
`--timezone +08:00` appends an explicit offset after normalization. If omitted,
timestamps remain timezone-unspecified.

## Parsing Architecture

The parser is split into these stages:

1. Container decoding selects the relevant view from plain, NUL-padded, or
   HTML/XMP-wrapped input.
2. Content scoring selects JSONL, application-export, Logback, Spark, or Flink framing.
3. Format-specific framers stream complete logical event views to their decoder.
4. Decoders map source headers into a shared `LogRecord`.
5. Semantic enrichers classify workflow attempts, retries, state transitions,
   HTTP/SQL activity, Spark application/job/stage/task lifecycle events, and Flink
   terminal job states.
6. The analyzer immediately filters and aggregates each canonical record.
7. Renderers produce text, Markdown, or JSON.

The source buffer owns all input bytes while decoding. Normal CLI analysis does
not retain a vector of every parsed record; it keeps aggregate counts, grouped
fingerprints, representative messages, and latency samples. Buffered and
memory-mapped input therefore have identical results, while `mmap` avoids the
initial whole-file copy.

## Outcome Semantics

Severity and incident outcome are deliberately separate:

- `WARN` and an exception do not necessarily mean a Spark application failed.
- Terminal workflow state or Spark/Flink application status has highest precedence.
- Failed jobs, stages, and tasks are stronger evidence than message keywords.
- Advisory warnings remain counted but do not become failure groups.
- When no terminal marker exists, failure events produce a conservative failed
  outcome with explicit fallback evidence.

This prevents a successful Spark application with noisy startup warnings from
being summarized as a failed incident.

## Extracted Signals

Application exports:

- trace/log ID
- flow and task-instance IDs
- workflow and step names/IDs
- retry round and persisted state
- multiline exceptions
- HTTP response latency and status
- SQL message family

Spark syslog:

- application ID and terminal status
- job, stage, task, and executor identifiers
- completed and failed lifecycle counts
- warning and advisory counts
- incidental exceptions
- selected elapsed-time metrics

Flink console logs:

- normalized time range and logger activity
- grouped Java exception root causes
- failover retry counts and warning patterns
- retry-exhaustion failure evidence when no explicit terminal marker exists
- terminal job states when present

## Output

`analyze` includes:

- detected input and container formats
- first and last normalized timestamps
- resolved outcome and supporting evidence
- physical-line and logical-event counts
- malformed and continuation counts
- failures, warnings, exceptions, advisories, and retries
- Spark and Flink execution lifecycle totals when present
- normalized failure, exception, and warning groups
- service/endpoint counts and latency percentiles in text and Markdown output
- logger activity and retry operations for Flink input

Markdown table cells are escaped, and JSON output is valid structured data rather
than a text report wrapped in JSON. Use `--limit` to bound the number of rows in
grouped output.

## Tests

The test suite includes sanitized fixtures modeled on:

- a reverse-ordered application export with repeated workflow failure and a
  terminal failed state
- a successful Spark/YARN HTML syslog with warnings, an incidental exception,
  completed job/stage/task events, and a thread name containing spaces
- a Flink `jobmanager.out` file with multiline exceptions, retries, warnings,
  and terminal job states
- a generated NUL-padded, full-year `jobmanager.log` variant with aligned logger
  and thread columns

No workplace hosts, users, service names, IDs, URLs, payloads, or internal code
are included in the fixtures.

## Current Boundaries

Implemented parsing is single-process and single-threaded. Each CLI invocation
accepts one file. Cross-file batching and any desired process-level concurrency
belong to the caller or LLM harness.

The parser aggregates records as they are decoded, but exact latency percentiles
still retain their numeric samples. Semantic fields, grouping keys, and
representative messages are owned strings.

The following remain separate milestones:

- cross-file correlation and incident graphs
- generic profile-defined extractors and arbitrary `--where field=value` queries
- request/response pairing across interleaved threads
- exact time-window baselines instead of chronological file halves
- bounded-memory approximate quantiles
- parallel boundary scanning and decoder shards
- configurable redaction policies
- trace and context CLI rendering
- benchmark and sanitizer automation

Correct framing and outcome resolution are treated as prerequisites for those
performance and correlation features.
