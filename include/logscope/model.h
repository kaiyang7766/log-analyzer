#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace logscope {

struct FieldConfig {
  std::string timestamp = "timestamp";
  std::string severity = "level";
  std::string service = "service";
  std::string trace_id = "trace_id";
  std::string latency_ms = "latency_ms";
  std::string message = "message";
  std::string endpoint = "endpoint";
  std::string stack_trace = "stack_trace";
};

struct Filters {
  std::string from;
  std::string to;
  std::string level;
  std::string service;
  std::string trace_id;
  std::string text;
};

enum class IoMode { mmap, buffered };
enum class OutputFormat { text, markdown, json };

struct AnalyzeOptions {
  FieldConfig fields;
  Filters filters;
  std::size_t threads = 1;
  IoMode io_mode = IoMode::buffered;
  bool retain_records = false;
};

struct LogRecord {
  std::uint64_t offset = 0;
  std::string timestamp;
  std::string severity;
  std::string service;
  std::string trace_id;
  std::string endpoint;
  std::string message;
  std::string stack_trace;
  double latency_ms = -1.0;
};

struct ErrorStats {
  std::uint64_t total = 0;
  std::uint64_t previous = 0;
  std::uint64_t recent = 0;
  std::unordered_map<std::string, std::uint64_t> services;
  std::unordered_map<std::string, std::uint64_t> endpoints;
  std::string example;
};

struct AnalysisResult {
  std::uint64_t lines = 0;
  std::uint64_t matched = 0;
  std::uint64_t malformed = 0;
  std::uint64_t bytes = 0;
  double elapsed_seconds = 0.0;
  std::unordered_map<std::string, std::uint64_t> services;
  std::unordered_map<std::string, std::uint64_t> endpoints;
  std::unordered_map<std::string, ErrorStats> errors;
  std::unordered_map<std::string, std::vector<double>> endpoint_latencies;
  std::unordered_map<std::string, std::vector<double>> service_latencies;
  std::vector<LogRecord> records;
};

struct Percentiles {
  std::size_t count = 0;
  double p50 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double max = 0.0;
};

}  // namespace logscope
