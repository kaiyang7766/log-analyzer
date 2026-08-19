#include "logscope/analyzer.h"

#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "input.h"

namespace logscope {
namespace {

struct Shard {
  std::uint64_t lines = 0;
  std::uint64_t matched = 0;
  std::uint64_t malformed = 0;
  std::unordered_map<std::string, std::uint64_t> services;
  std::unordered_map<std::string, std::uint64_t> endpoints;
  std::unordered_map<std::string, ErrorStats> errors;
  std::unordered_map<std::string, std::vector<double>> endpoint_latencies;
  std::unordered_map<std::string, std::vector<double>> service_latencies;
  std::vector<LogRecord> records;
};

std::string lower(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return result;
}

bool contains_case_insensitive(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  return lower(haystack).find(lower(needle)) != std::string::npos;
}

std::string string_field(const simdjson::dom::object& object, std::string_view key) {
  simdjson::dom::element element;
  if (object.at_key(key).get(element)) return {};
  std::string_view text;
  if (!element.get_string().get(text)) return std::string(text);
  std::int64_t integer = 0;
  if (!element.get_int64().get(integer)) return std::to_string(integer);
  std::uint64_t unsigned_integer = 0;
  if (!element.get_uint64().get(unsigned_integer)) return std::to_string(unsigned_integer);
  double number = 0.0;
  if (!element.get_double().get(number)) return std::to_string(number);
  return {};
}

double number_field(const simdjson::dom::object& object, std::string_view key) {
  simdjson::dom::element element;
  if (object.at_key(key).get(element)) return -1.0;
  double number = 0.0;
  if (!element.get_double().get(number)) return number;
  std::string_view text;
  if (!element.get_string().get(text)) {
    try {
      std::size_t consumed = 0;
      const auto parsed = std::stod(std::string(text), &consumed);
      if (consumed == text.size()) return parsed;
    } catch (...) {
    }
  }
  return -1.0;
}

bool is_error(std::string_view severity, std::string_view message, std::string_view stack) {
  const auto level = lower(severity);
  return level == "error" || level == "fatal" || level == "critical" ||
         !stack.empty() || contains_case_insensitive(message, "error") ||
         contains_case_insensitive(message, "exception") ||
         contains_case_insensitive(message, "timeout") ||
         contains_case_insensitive(message, "failed");
}

bool passes_filters(const LogRecord& record, const Filters& filters) {
  if (!filters.from.empty() && record.timestamp < filters.from) return false;
  if (!filters.to.empty() && record.timestamp > filters.to) return false;
  if (!filters.level.empty() && lower(record.severity) != lower(filters.level)) return false;
  if (!filters.service.empty() && record.service != filters.service) return false;
  if (!filters.trace_id.empty() && record.trace_id != filters.trace_id) return false;
  if (!filters.text.empty() &&
      !contains_case_insensitive(record.message, filters.text) &&
      !contains_case_insensitive(record.stack_trace, filters.text)) return false;
  return true;
}

void consume_record(Shard& shard, LogRecord record, const AnalyzeOptions& options,
                    std::size_t input_size) {
  if (!passes_filters(record, options.filters)) return;
  ++shard.matched;

  const auto service = record.service.empty() ? "<unknown>" : record.service;
  const auto endpoint = record.endpoint.empty() ? "<unknown>" : record.endpoint;
  ++shard.services[service];
  ++shard.endpoints[endpoint];

  if (record.latency_ms >= 0.0 && std::isfinite(record.latency_ms)) {
    shard.endpoint_latencies[endpoint].push_back(record.latency_ms);
    shard.service_latencies[service].push_back(record.latency_ms);
  }

  if (is_error(record.severity, record.message, record.stack_trace)) {
    const auto normalized = normalize_error(record.message, record.stack_trace);
    auto& stats = shard.errors[normalized];
    ++stats.total;
    if (record.offset >= input_size / 2) ++stats.recent;
    else ++stats.previous;
    ++stats.services[service];
    ++stats.endpoints[endpoint];
    if (stats.example.empty()) stats.example = record.message;
  }

  if (options.retain_records) shard.records.push_back(std::move(record));
}

void parse_range(const char* data, std::size_t size, std::size_t nominal_begin,
                 std::size_t nominal_end, const AnalyzeOptions& options, Shard& shard) {
  std::size_t begin = nominal_begin;
  if (begin != 0) {
    while (begin < size && data[begin - 1] != '\n') ++begin;
  }

  simdjson::dom::parser parser;
  std::size_t cursor = begin;
  while (cursor < size && cursor < nominal_end) {
    const auto line_start = cursor;
    while (cursor < size && data[cursor] != '\n') ++cursor;
    std::string_view line(data + line_start, cursor - line_start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    ++shard.lines;
    if (!line.empty()) {
      simdjson::padded_string padded(line);
      simdjson::dom::element document;
      simdjson::dom::object object;
      if (parser.parse(padded).get(document) || document.get_object().get(object)) {
        ++shard.malformed;
      } else {
        LogRecord record;
        record.offset = line_start;
        record.timestamp = string_field(object, options.fields.timestamp);
        record.severity = string_field(object, options.fields.severity);
        record.service = string_field(object, options.fields.service);
        record.trace_id = string_field(object, options.fields.trace_id);
        record.endpoint = string_field(object, options.fields.endpoint);
        record.message = string_field(object, options.fields.message);
        record.stack_trace = string_field(object, options.fields.stack_trace);
        record.latency_ms = number_field(object, options.fields.latency_ms);
        consume_record(shard, std::move(record), options, size);
      }
    }
    if (cursor < size) ++cursor;
  }
}

template <typename Map>
void merge_counts(Map& destination, const Map& source) {
  for (const auto& [key, value] : source) destination[key] += value;
}

template <typename Map>
void merge_vectors(Map& destination, Map& source) {
  for (auto& [key, values] : source) {
    auto& target = destination[key];
    target.insert(target.end(), std::make_move_iterator(values.begin()),
                  std::make_move_iterator(values.end()));
  }
}

}  // namespace

AnalysisResult analyze_file(const std::filesystem::path& path, const AnalyzeOptions& options) {
  const auto started = std::chrono::steady_clock::now();
  detail::InputBuffer input(path, options.io_mode);

  AnalysisResult result;
  result.bytes = input.size();
  if (input.size() == 0) return result;

  // Day 1 deliberately runs one shard. Day 2 can schedule this range parser
  // across std::jthreads without changing the parsing or aggregation semantics.
  std::vector<Shard> shards(1);
  parse_range(input.data(), input.size(), 0, input.size(), options, shards.front());

  for (auto& shard : shards) {
    result.lines += shard.lines;
    result.matched += shard.matched;
    result.malformed += shard.malformed;
    merge_counts(result.services, shard.services);
    merge_counts(result.endpoints, shard.endpoints);
    merge_vectors(result.endpoint_latencies, shard.endpoint_latencies);
    merge_vectors(result.service_latencies, shard.service_latencies);
    for (auto& [key, stats] : shard.errors) {
      auto& target = result.errors[key];
      target.total += stats.total;
      target.previous += stats.previous;
      target.recent += stats.recent;
      merge_counts(target.services, stats.services);
      merge_counts(target.endpoints, stats.endpoints);
      if (target.example.empty()) target.example = std::move(stats.example);
    }
    result.records.insert(result.records.end(), std::make_move_iterator(shard.records.begin()),
                          std::make_move_iterator(shard.records.end()));
  }
  std::sort(result.records.begin(), result.records.end(),
            [](const LogRecord& left, const LogRecord& right) { return left.offset < right.offset; });
  result.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  return result;
}

std::vector<LogRecord> select_trace(const AnalysisResult& result, std::string_view trace_id) {
  std::vector<LogRecord> records;
  for (const auto& record : result.records) {
    if (record.trace_id == trace_id) records.push_back(record);
  }
  return records;
}

std::vector<std::vector<LogRecord>> select_contexts(const AnalysisResult& result,
                                                    std::string_view needle,
                                                    std::size_t before,
                                                    std::size_t after) {
  std::vector<std::vector<LogRecord>> contexts;
  for (std::size_t index = 0; index < result.records.size(); ++index) {
    const auto& record = result.records[index];
    if (!contains_case_insensitive(record.message, needle) &&
        !contains_case_insensitive(record.stack_trace, needle)) continue;
    const auto start = index > before ? index - before : 0;
    const auto end = std::min(result.records.size(), index + after + 1);
    contexts.emplace_back(result.records.begin() + static_cast<std::ptrdiff_t>(start),
                          result.records.begin() + static_cast<std::ptrdiff_t>(end));
  }
  return contexts;
}

}  // namespace logscope
