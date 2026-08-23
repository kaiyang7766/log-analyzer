#include "logscope/analyzer.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "formats.h"
#include "input.h"

namespace logscope {
namespace {

struct Shard {
  std::uint64_t matched = 0;
  std::uint64_t warnings = 0;
  std::uint64_t error_events = 0;
  std::uint64_t exception_events = 0;
  std::uint64_t advisory_events = 0;
  std::uint64_t completed_jobs = 0;
  std::uint64_t completed_stages = 0;
  std::uint64_t completed_tasks = 0;
  std::uint64_t failed_jobs = 0;
  std::uint64_t failed_stages = 0;
  std::uint64_t failed_tasks = 0;
  bool saw_terminal_success = false;
  bool saw_terminal_failure = false;
  bool saw_failure_evidence = false;
  std::string terminal_evidence;
  std::unordered_map<std::string, std::uint32_t> retry_rounds;
  std::unordered_map<std::string, std::uint64_t> services;
  std::unordered_map<std::string, std::uint64_t> endpoints;
  std::unordered_map<std::string, std::uint64_t> loggers;
  std::unordered_map<std::string, std::uint64_t> states;
  std::unordered_map<std::string, std::uint64_t> warning_patterns;
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

bool passes_filters(const LogRecord& record, const Filters& filters) {
  if (!filters.from.empty() && record.timestamp < filters.from) return false;
  if (!filters.to.empty() && record.timestamp > filters.to) return false;
  if (!filters.level.empty() && lower(record.severity) != lower(filters.level)) return false;
  if (!filters.service.empty() && record.service != filters.service) return false;
  if (!filters.trace_id.empty() && record.trace_id != filters.trace_id) return false;
  if (!filters.text.empty() &&
      !contains_case_insensitive(record.message, filters.text) &&
      !contains_case_insensitive(record.stack_trace, filters.text))
    return false;
  return true;
}

std::string compact_evidence(std::string value, std::size_t limit = 240) {
  std::replace(value.begin(), value.end(), '\n', ' ');
  std::replace(value.begin(), value.end(), '\r', ' ');
  if (value.size() > limit) value = value.substr(0, limit - 3) + "...";
  return value;
}

void consume_record(Shard& shard, LogRecord record, const AnalyzeOptions& options,
                    std::size_t input_size, bool reverse_order) {
  if (record.failure) shard.saw_failure_evidence = true;
  if (record.terminal_success) {
    shard.saw_terminal_success = true;
    if (!shard.saw_terminal_failure)
      shard.terminal_evidence = compact_evidence(record.message);
  }
  if (record.terminal_failure) {
    shard.saw_terminal_failure = true;
    shard.terminal_evidence = compact_evidence(record.message);
  }

  if (!passes_filters(record, options.filters)) return;
  ++shard.matched;

  const auto service = record.service.empty() ? "<unknown>" : record.service;
  const auto endpoint = record.endpoint.empty() ? "<unknown>" : record.endpoint;
  ++shard.services[service];
  ++shard.endpoints[endpoint];
  if (!record.logger.empty()) ++shard.loggers[record.logger];

  const auto level = lower(record.severity);
  if (level == "warn" || level == "warning") {
    ++shard.warnings;
    ++shard.warning_patterns[normalize_error(record.message)];
  }
  if (record.failure) ++shard.error_events;
  if (record.exception) ++shard.exception_events;
  if (record.advisory) ++shard.advisory_events;
  if (record.retry_round > 0 && record.event_kind == "workflow_attempt") {
    std::string retry_key;
    for (const auto key : {"flow_id", "task_instance_id", "step_id"}) {
      if (const auto value = record.attributes.find(key);
          value != record.attributes.end()) {
        retry_key += value->second;
        retry_key.push_back(':');
      }
    }
    if (retry_key.empty()) retry_key = record.endpoint;
    shard.retry_rounds[retry_key] =
        std::max(shard.retry_rounds[retry_key], record.retry_round);
  }

  if (const auto state = record.attributes.find("state");
      state != record.attributes.end())
    ++shard.states[state->second];

  if (record.event_kind == "job_completed") ++shard.completed_jobs;
  else if (record.event_kind == "stage_completed") ++shard.completed_stages;
  else if (record.event_kind == "task_completed") ++shard.completed_tasks;
  else if (record.event_kind == "job_failed") ++shard.failed_jobs;
  else if (record.event_kind == "stage_failed") ++shard.failed_stages;
  else if (record.event_kind == "task_failed") ++shard.failed_tasks;

  if (record.latency_ms >= 0.0 && std::isfinite(record.latency_ms)) {
    shard.endpoint_latencies[endpoint].push_back(record.latency_ms);
    shard.service_latencies[service].push_back(record.latency_ms);
  }

  if (record.failure) {
    const auto normalized = normalize_error(record.message, record.stack_trace);
    auto& stats = shard.errors[normalized];
    ++stats.total;
    const bool in_later_half = record.offset >= input_size / 2;
    const bool recent = reverse_order ? !in_later_half : in_later_half;
    if (recent)
      ++stats.recent;
    else
      ++stats.previous;
    ++stats.services[service];
    ++stats.endpoints[endpoint];
    if (stats.example.empty()) stats.example = record.message;
  }

  if (options.retain_records) shard.records.push_back(std::move(record));
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

void merge_shard(AnalysisResult& result, Shard& shard) {
  result.matched += shard.matched;
  result.warnings += shard.warnings;
  result.error_events += shard.error_events;
  result.exception_events += shard.exception_events;
  result.advisory_events += shard.advisory_events;
  for (const auto& [_, round] : shard.retry_rounds)
    result.retries += round > 0 ? round - 1 : 0;
  result.completed_jobs += shard.completed_jobs;
  result.completed_stages += shard.completed_stages;
  result.completed_tasks += shard.completed_tasks;
  result.failed_jobs += shard.failed_jobs;
  result.failed_stages += shard.failed_stages;
  result.failed_tasks += shard.failed_tasks;
  merge_counts(result.services, shard.services);
  merge_counts(result.endpoints, shard.endpoints);
  merge_counts(result.loggers, shard.loggers);
  merge_counts(result.states, shard.states);
  merge_counts(result.warning_patterns, shard.warning_patterns);
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
  result.records.insert(result.records.end(),
                        std::make_move_iterator(shard.records.begin()),
                        std::make_move_iterator(shard.records.end()));

  if (shard.saw_terminal_failure) {
    result.outcome = "failed";
    result.outcome_evidence = std::move(shard.terminal_evidence);
  } else if (shard.saw_terminal_success && result.outcome != "failed") {
    result.outcome = "succeeded";
    result.outcome_evidence = std::move(shard.terminal_evidence);
  } else if (shard.saw_failure_evidence && result.outcome == "unknown") {
    result.outcome = "failed";
    result.outcome_evidence =
        "No terminal status was found; failure events were present.";
  }
}

}  // namespace

AnalysisResult analyze_file(const std::filesystem::path& path,
                            const AnalyzeOptions& options) {
  const auto started = std::chrono::steady_clock::now();
  detail::InputBuffer input(path, options.io_mode);

  AnalysisResult result;
  result.source_file = path.filename().string();
  result.bytes = input.size();
  if (input.size() == 0) return result;

  auto decoded =
      detail::decode_input(std::string_view(input.data(), input.size()), options);
  result.lines = decoded.physical_lines;
  result.events = decoded.logical_events;
  result.continuations = decoded.continuation_lines;
  result.malformed = decoded.malformed;
  result.detected_format = std::move(decoded.format_name);
  result.container_format = std::move(decoded.container_name);

  Shard shard;
  for (auto& record : decoded.records)
    consume_record(shard, std::move(record), options, input.size(),
                   decoded.reverse_order);
  merge_shard(result, shard);

  if (result.outcome == "unknown" && result.matched > 0) {
    result.outcome_evidence =
        "No terminal success or failure marker was found.";
  }

  std::stable_sort(result.records.begin(), result.records.end(),
                   [](const LogRecord& left, const LogRecord& right) {
                     if (left.timestamp != right.timestamp)
                       return left.timestamp < right.timestamp;
                     return left.offset < right.offset;
                   });
  result.elapsed_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  return result;
}

std::vector<LogRecord> select_trace(const AnalysisResult& result,
                                    std::string_view trace_id) {
  std::vector<LogRecord> records;
  for (const auto& record : result.records) {
    if (record.trace_id == trace_id) records.push_back(record);
  }
  return records;
}

std::vector<std::vector<LogRecord>> select_contexts(
    const AnalysisResult& result, std::string_view needle, std::size_t before,
    std::size_t after) {
  std::vector<std::vector<LogRecord>> contexts;
  for (std::size_t index = 0; index < result.records.size(); ++index) {
    const auto& record = result.records[index];
    if (!contains_case_insensitive(record.message, needle) &&
        !contains_case_insensitive(record.stack_trace, needle))
      continue;
    const auto start = index > before ? index - before : 0;
    const auto end = std::min(result.records.size(), index + after + 1);
    contexts.emplace_back(
        result.records.begin() + static_cast<std::ptrdiff_t>(start),
        result.records.begin() + static_cast<std::ptrdiff_t>(end));
  }
  return contexts;
}

}  // namespace logscope
