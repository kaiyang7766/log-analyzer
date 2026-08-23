# LogScope++

LogScope++ is a local C++20 incident-log analyzer. It parses heterogeneous exported
logs without uploading them, reconstructs multiline records, extracts workflow,
Spark, and Flink lifecycle signals, groups failures, and produces text, Markdown, or JSON
summaries.

## Supported Inputs

Auto detection currently recognizes:

- JSONL objects with configurable field names
- enriched application exports with an outer metadata envelope and `_msg=`
- raw Logback-style application logs
- raw Spark/YARN syslog
- Flink JobManager console logs such as `jobmanager.out`
- browser-saved Spark syslog wrapped in an HTML `<xmp>` element

Use `--input-format` when automatic detection is ambiguous:

```bash
--input-format auto|jsonl|app-export|logback|spark-syslog|flink-console
```

Multiline stack traces and diagnostic blocks are attached to the preceding event.
Blank lines are not treated as record boundaries.

## Build

Requirements: CMake 3.20+, a C++20 compiler, and simdjson.

```bash
# macOS
brew install cmake simdjson

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

Common filters:

```text
--from TIMESTAMP
--to TIMESTAMP
--level LEVEL
--service SERVICE
--trace-id ID
--text TEXT
```

Spark and Flink console timestamps have no timezone in the source format.
`--timezone +08:00` appends an explicit offset after normalization. If omitted,
timestamps remain timezone-unspecified.

## Parsing Architecture

The parser is split into these stages:

1. Container decoding extracts raw text from plain files or HTML/XMP wrappers.
2. Content scoring selects JSONL, application-export, Logback, Spark, or Flink framing.
3. Format-specific framers create complete logical events before parsing fields.
4. Decoders map source headers into a shared `LogRecord`.
5. Semantic enrichers classify workflow attempts, retries, state transitions,
   HTTP/SQL activity, Spark application/job/stage/task lifecycle events, and Flink
   terminal job states.
6. The analyzer filters and aggregates the canonical records.
7. Renderers produce text, Markdown, or JSON.

The source buffer owns all input bytes while decoding. Records own the fields
needed after analysis, so buffered and memory-mapped input have identical
lifetime semantics.

## Outcome Semantics

Severity and incident outcome are deliberately separate:

- `WARN` and an exception do not necessarily mean a Spark application failed.
- terminal workflow state or Spark application status has highest precedence.
- failed Spark jobs, stages, and tasks are stronger evidence than message keywords.
- advisory warnings remain counted but do not become failure groups.
- when no terminal marker exists, failure events produce a conservative failed
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
- terminal job states when present

## Output

`analyze` includes:

- detected input and container formats
- first and last normalized timestamps
- resolved outcome and supporting evidence
- physical-line and logical-event counts
- malformed and continuation counts
- failures, warnings, exceptions, advisories, and retries
- Spark execution lifecycle totals when present
- normalized failure, exception, and warning groups
- service/endpoint counts and latency percentiles

Markdown table cells are escaped, and JSON output is valid structured data rather
than a text report wrapped in JSON.

## Tests

The test suite includes sanitized fixtures modeled on:

- a reverse-ordered application export with repeated workflow failure and a
  terminal failed state
- a successful Spark/YARN HTML syslog with warnings, an incidental exception,
  completed job/stage/task events, and a thread name containing spaces

No workplace hosts, users, service names, IDs, URLs, payloads, or internal code
are included in the fixtures.

## Current Boundaries

Implemented parsing is single-process and single-threaded. `mmap` avoids an input
copy but semantic fields and retained records are still owned strings.

The following remain separate milestones:

- cross-file correlation graphs between migration and Spark bundles
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
