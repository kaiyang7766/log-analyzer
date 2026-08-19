# LogScope++

LogScope++ is a local C++20 command-line analyzer for JSONL incident logs. The current **Day 1 build** reads logs without uploading them, filters events, groups normalized errors, and calculates endpoint or service latency percentiles.

## Build

Requirements: CMake 3.20+, a C++20 compiler, and simdjson.

```bash
# macOS
brew install cmake simdjson

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On Debian/Ubuntu, install `cmake`, `ninja-build`, `g++`, and `libsimdjson-dev` instead.

## Run

```bash
./build/logscope analyze examples/incident.jsonl
./build/logscope errors examples/incident.jsonl --group-by service
./build/logscope latency examples/incident.jsonl --group-by endpoint
./build/logscope analyze examples/incident.jsonl --trace-id abc123
./build/logscope errors examples/incident.jsonl --service orders --from 2026-08-20T10:01:00Z
./build/logscope analyze examples/incident.jsonl --text timeout --format markdown
```

Run `./build/logscope --help` for all filters. ISO-8601 timestamps work naturally because the Day 1 implementation compares timestamp strings; exported logs should use one consistent timezone and format.

Expected fields are `timestamp`, `level`, `service`, `trace_id`, `latency_ms`, `message`, `endpoint`, and optional `stack_trace`. Malformed lines are counted and skipped. Error normalization collapses changing UUIDs, addresses, IPs, and numbers; the most recent half of the file is compared with the first half.

## Scope

Implemented now:

- JSONL parsing with simdjson and malformed-line handling
- time, level, service, trace-ID, and text filters
- normalized error grouping, including recent-versus-previous counts
- service and endpoint counts
- p50/p95/p99/max latency grouped by endpoint or service
- plain-text and Markdown summaries

Deferred to later milestones: trace reconstruction, surrounding context search, configurable field mappings, parallel parsing and sharded maps, memory-mapped I/O benchmarks, JSON export, synthetic multi-GB data generation, and Linux `perf` profiling.
