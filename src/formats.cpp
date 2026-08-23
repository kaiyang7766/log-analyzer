#include "formats.h"

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logscope::detail {
namespace {

struct ContainerView {
  std::string_view content;
  std::size_t base_offset = 0;
  std::string name = "plain-text";
};

struct EventSpan {
  std::size_t offset = 0;
  std::string_view header;
  std::string_view continuation;
};

std::string lower(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return result;
}

std::string_view trim_view(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return value;
}

bool contains_case_insensitive(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  return lower(haystack).find(lower(needle)) != std::string::npos;
}

bool is_level(std::string_view value) {
  static constexpr std::array<std::string_view, 7> levels{
      "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "CRITICAL"};
  return std::find(levels.begin(), levels.end(), value) != levels.end();
}

std::string_view take_token(std::string_view value, std::size_t& cursor) {
  while (cursor < value.size() &&
         std::isspace(static_cast<unsigned char>(value[cursor])))
    ++cursor;
  const auto start = cursor;
  while (cursor < value.size() &&
         !std::isspace(static_cast<unsigned char>(value[cursor])))
    ++cursor;
  return value.substr(start, cursor - start);
}

std::string_view take_bracket(std::string_view value, std::size_t& cursor) {
  while (cursor < value.size() &&
         std::isspace(static_cast<unsigned char>(value[cursor])))
    ++cursor;
  if (cursor >= value.size() || value[cursor] != '[') return {};
  const auto end = value.find(']', cursor + 1);
  if (end == std::string_view::npos) return {};
  const auto result = value.substr(cursor + 1, end - cursor - 1);
  cursor = end + 1;
  return result;
}

bool all_digits(std::string_view value) {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char ch) { return std::isdigit(ch); });
}

std::uint32_t unsigned_after(std::string_view value, std::string_view marker) {
  const auto marker_position = value.find(marker);
  if (marker_position == std::string_view::npos) return 0;
  auto position = marker_position + marker.size();
  while (position < value.size() &&
         std::isspace(static_cast<unsigned char>(value[position])))
    ++position;
  std::uint32_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data() + position, value.data() + value.size(), parsed);
  return error == std::errc{} && end != value.data() + position ? parsed : 0;
}

double number_after(std::string_view value, std::string_view marker) {
  const auto marker_position = value.find(marker);
  if (marker_position == std::string_view::npos) return -1.0;
  auto position = marker_position + marker.size();
  while (position < value.size() &&
         std::isspace(static_cast<unsigned char>(value[position])))
    ++position;
  const auto start = position;
  while (position < value.size() &&
         (std::isdigit(static_cast<unsigned char>(value[position])) ||
          value[position] == '.'))
    ++position;
  if (position == start) return -1.0;
  try {
    return std::stod(std::string(value.substr(start, position - start)));
  } catch (...) {
    return -1.0;
  }
}

std::string token_after(std::string_view value, std::string_view marker) {
  const auto marker_position = value.find(marker);
  if (marker_position == std::string_view::npos) return {};
  auto position = marker_position + marker.size();
  while (position < value.size() &&
         std::isspace(static_cast<unsigned char>(value[position])))
    ++position;
  const auto start = position;
  while (position < value.size() &&
         !std::isspace(static_cast<unsigned char>(value[position])) &&
         value[position] != ',' && value[position] != ')' && value[position] != ']')
    ++position;
  return std::string(value.substr(start, position - start));
}

std::string extract_identifier(std::string_view value, std::string_view prefix) {
  const auto position = value.find(prefix);
  if (position == std::string_view::npos) return {};
  auto end = position + prefix.size();
  while (end < value.size() &&
         (std::isalnum(static_cast<unsigned char>(value[end])) ||
          value[end] == '_' || value[end] == '-'))
    ++end;
  return std::string(value.substr(position, end - position));
}

std::string continuation_text(std::string_view continuation) {
  std::string result;
  std::size_t cursor = 0;
  while (cursor < continuation.size()) {
    const auto end = continuation.find('\n', cursor);
    auto line = continuation.substr(
        cursor, end == std::string_view::npos ? continuation.size() - cursor : end - cursor);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    line = trim_view(line);
    if (!line.empty()) {
      if (!result.empty()) result.push_back('\n');
      result.append(line);
    }
    if (end == std::string_view::npos) break;
    cursor = end + 1;
  }
  return result;
}

bool has_exception_line(std::string_view value) {
  if (value.find("Caused by:") != std::string_view::npos) return true;
  std::size_t cursor = 0;
  while (cursor < value.size()) {
    const auto end = value.find('\n', cursor);
    const auto line = trim_view(value.substr(
        cursor, end == std::string_view::npos ? value.size() - cursor : end - cursor));
    const auto colon = line.find(':');
    const auto type = colon == std::string_view::npos ? line : line.substr(0, colon);
    if (type.ends_with("Exception") || type.ends_with("Error")) return true;
    if (end == std::string_view::npos) break;
    cursor = end + 1;
  }
  return false;
}

std::string normalize_app_timestamp(std::string_view date, std::string_view time) {
  if (date.size() != 10) return std::string(date) + " " + std::string(time);
  return std::string(date) + "T" + std::string(time);
}

std::string normalize_short_year_timestamp(std::string_view value,
                                           std::string_view default_timezone) {
  if (value.size() < 17) return std::string(value);
  std::string result = "20";
  result.append(value.substr(0, 2));
  result.push_back('-');
  result.append(value.substr(3, 2));
  result.push_back('-');
  result.append(value.substr(6, 2));
  result.push_back('T');
  result.append(value.substr(9));
  result.append(default_timezone);
  return result;
}

ContainerView unwrap_container(std::string_view input) {
  const auto xmp = input.find("<xmp");
  if (xmp == std::string_view::npos) return {input, 0, "plain-text"};
  const auto start_tag_end = input.find('>', xmp);
  const auto end = input.find("</xmp>", start_tag_end);
  if (start_tag_end == std::string_view::npos || end == std::string_view::npos ||
      end < start_tag_end)
    throw std::runtime_error("invalid HTML log wrapper: unterminated xmp element");
  const auto start = start_tag_end + 1;
  return {input.substr(start, end - start), start, "html-xmp"};
}

bool looks_like_app_header(std::string_view line) {
  std::size_t cursor = 0;
  const auto level = take_token(line, cursor);
  const auto date = take_token(line, cursor);
  const auto time = take_token(line, cursor);
  return is_level(level) && date.size() == 10 && date[4] == '-' && date[7] == '-' &&
         time.size() >= 12 && time[2] == ':' && time[5] == ':';
}

bool parse_spark_header(std::string_view line, std::string_view default_timezone,
                        LogRecord* record) {
  if (line.size() < 27 || line[2] != '/' || line[5] != '/' || line[8] != ' ' ||
      line[11] != ':' || line[14] != ':' || line[17] != '.')
    return false;

  const auto timestamp = line.substr(0, 21);
  const auto remainder = line.substr(22);
  std::size_t level_position = std::string_view::npos;
  std::string_view level;
  static constexpr std::array<std::string_view, 6> levels{
      " TRACE ", " DEBUG ", " INFO ", " WARN ", " ERROR ", " FATAL "};
  for (const auto candidate : levels) {
    const auto position = remainder.find(candidate);
    if (position != std::string_view::npos &&
        (level_position == std::string_view::npos || position < level_position)) {
      level_position = position;
      level = candidate.substr(1, candidate.size() - 2);
    }
  }
  if (level_position == std::string_view::npos) return false;

  const auto logger_start = level_position + level.size() + 2;
  const auto colon = remainder.find(':', logger_start);
  if (colon == std::string_view::npos) return false;
  if (!record) return true;

  record->timestamp = normalize_short_year_timestamp(timestamp, default_timezone);
  record->thread = std::string(trim_view(remainder.substr(0, level_position)));
  record->severity = std::string(level);
  record->logger =
      std::string(trim_view(remainder.substr(logger_start, colon - logger_start)));
  record->source = record->logger;
  record->service = "spark";
  record->message = std::string(trim_view(remainder.substr(colon + 1)));
  return true;
}

bool parse_flink_header(std::string_view line, std::string_view default_timezone,
                        LogRecord* record) {
  if (line.size() < 24 || line[2] != '/' || line[5] != '/' || line[8] != ' ' ||
      line[11] != ':' || line[14] != ':' || line[17] != ' ')
    return false;

  const auto timestamp = line.substr(0, 17);
  const auto remainder = line.substr(18);
  std::size_t cursor = 0;
  const auto level = take_token(remainder, cursor);
  if (!is_level(level)) return false;

  while (cursor < remainder.size() &&
         std::isspace(static_cast<unsigned char>(remainder[cursor])))
    ++cursor;
  const auto colon = remainder.find(':', cursor);
  if (colon == std::string_view::npos || colon == cursor) return false;
  if (!record) return true;

  record->timestamp =
      normalize_short_year_timestamp(timestamp, default_timezone);
  record->severity = std::string(level);
  record->logger = std::string(trim_view(remainder.substr(cursor, colon - cursor)));
  record->source = record->logger;
  record->service = "flink";
  record->message = std::string(trim_view(remainder.substr(colon + 1)));
  return true;
}

InputFormat detect_format(std::string_view content, InputFormat requested) {
  if (requested != InputFormat::auto_detect) return requested;

  std::size_t cursor = 0;
  std::size_t app_headers = 0;
  std::size_t export_headers = 0;
  std::size_t spark_headers = 0;
  std::size_t flink_headers = 0;
  std::size_t examined = 0;
  while (cursor < content.size() && examined < 200) {
    const auto end = content.find('\n', cursor);
    auto line = content.substr(
        cursor, end == std::string_view::npos ? content.size() - cursor : end - cursor);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!trim_view(line).empty()) {
      ++examined;
      if (looks_like_app_header(line)) {
        ++app_headers;
        if (line.find("_msg=") != std::string_view::npos) ++export_headers;
      }
      if (parse_spark_header(line, {}, nullptr)) ++spark_headers;
      if (parse_flink_header(line, {}, nullptr)) ++flink_headers;
      if (examined == 1 && trim_view(line).starts_with('{')) return InputFormat::jsonl;
    }
    if (end == std::string_view::npos) break;
    cursor = end + 1;
  }

  if (spark_headers >= 2 && spark_headers >= app_headers) return InputFormat::spark_syslog;
  if (flink_headers >= 2 && flink_headers >= app_headers) return InputFormat::flink_console;
  if (export_headers >= 1) return InputFormat::app_export;
  if (app_headers >= 1) return InputFormat::logback;
  throw std::runtime_error(
      "could not detect input format; use --input-format to select one explicitly");
}

template <typename Predicate>
std::vector<EventSpan> frame_events(std::string_view content, std::size_t base_offset,
                                    Predicate is_start, DecodedInput& decoded) {
  std::vector<EventSpan> events;
  std::size_t cursor = 0;
  std::size_t event_start = std::string_view::npos;
  std::size_t header_end = 0;

  const auto flush = [&](std::size_t event_end) {
    if (event_start == std::string_view::npos) return;
    auto header = content.substr(event_start, header_end - event_start);
    if (!header.empty() && header.back() == '\r') header.remove_suffix(1);
    auto continuation_start = header_end;
    if (continuation_start < event_end && content[continuation_start] == '\n')
      ++continuation_start;
    events.push_back(
        {base_offset + event_start, header,
         content.substr(continuation_start, event_end - continuation_start)});
  };

  while (cursor < content.size()) {
    const auto line_start = cursor;
    const auto end = content.find('\n', cursor);
    const auto line_end = end == std::string_view::npos ? content.size() : end;
    auto line = content.substr(line_start, line_end - line_start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    ++decoded.physical_lines;
    if (is_start(line)) {
      flush(line_start);
      event_start = line_start;
      header_end = line_end;
    } else if (event_start != std::string_view::npos) {
      if (!trim_view(line).empty()) ++decoded.continuation_lines;
    } else if (!trim_view(line).empty()) {
      ++decoded.malformed;
    }
    if (end == std::string_view::npos) {
      cursor = content.size();
    } else {
      cursor = end + 1;
    }
  }
  flush(content.size());
  decoded.logical_events = events.size();
  return events;
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

bool message_looks_failed(std::string_view value) {
  return contains_case_insensitive(value, "error") ||
         contains_case_insensitive(value, "exception") ||
         contains_case_insensitive(value, "timeout") ||
         contains_case_insensitive(value, "failed");
}

void decode_jsonl(std::string_view content, std::size_t base_offset,
                  const AnalyzeOptions& options, DecodedInput& decoded) {
  simdjson::dom::parser parser;
  std::size_t cursor = 0;
  while (cursor < content.size()) {
    const auto line_start = cursor;
    const auto end = content.find('\n', cursor);
    const auto line_end = end == std::string_view::npos ? content.size() : end;
    auto line = content.substr(line_start, line_end - line_start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    ++decoded.physical_lines;
    if (!trim_view(line).empty()) {
      ++decoded.logical_events;
      simdjson::padded_string padded(line);
      simdjson::dom::element document;
      simdjson::dom::object object;
      if (parser.parse(padded).get(document) || document.get_object().get(object)) {
        ++decoded.malformed;
      } else {
        LogRecord record;
        record.offset = base_offset + line_start;
        record.timestamp = string_field(object, options.fields.timestamp);
        record.severity = string_field(object, options.fields.severity);
        record.service = string_field(object, options.fields.service);
        record.trace_id = string_field(object, options.fields.trace_id);
        record.endpoint = string_field(object, options.fields.endpoint);
        record.message = string_field(object, options.fields.message);
        record.stack_trace = string_field(object, options.fields.stack_trace);
        record.latency_ms = number_field(object, options.fields.latency_ms);
        const auto level = lower(record.severity);
        record.exception = has_exception_line(record.stack_trace) ||
                           contains_case_insensitive(record.message, "exception");
        record.failure = level == "error" || level == "fatal" || level == "critical" ||
                         !record.stack_trace.empty() || message_looks_failed(record.message);
        record.event_kind = record.failure ? "failure" : "log";
        decoded.records.push_back(std::move(record));
      }
    }
    cursor = end == std::string_view::npos ? content.size() : end + 1;
  }
}

void parse_named_bracket(std::string_view content, std::string_view name_key,
                         std::string_view id_key, LogRecord& record) {
  const auto colon = content.rfind(':');
  if (colon == std::string_view::npos || !all_digits(content.substr(colon + 1))) return;
  const auto name = trim_view(content.substr(0, colon));
  if (name.empty()) return;
  record.attributes[std::string(name_key)] = std::string(name);
  record.attributes[std::string(id_key)] = std::string(content.substr(colon + 1));
}

void enrich_application_record(LogRecord& record) {
  auto message = std::string_view(record.message);
  std::size_t prefix_cursor = 0;
  const auto first = take_bracket(message, prefix_cursor);
  const auto second = take_bracket(message, prefix_cursor);
  if (!first.empty() && !second.empty()) {
    parse_named_bracket(first, "workflow_name", "workflow_id", record);
    parse_named_bracket(second, "step_name", "step_id", record);
    if (record.attributes.contains("step_name"))
      record.endpoint = record.attributes["step_name"];
    record.message = std::string(trim_view(message.substr(prefix_cursor)));
    message = record.message;
  }

  record.retry_round = unsigned_after(message, "round:");
  if (message.find("Executing task") != std::string_view::npos) {
    record.event_kind = "workflow_attempt";
  } else if (message.find("Persisting task, state ") != std::string_view::npos) {
    record.event_kind = "state_transition";
    const auto state = token_after(message, "Persisting task, state ");
    if (!state.empty()) record.attributes["state"] = state;
    if (lower(state) == "failed") {
      record.failure = true;
      record.terminal_failure = true;
    }
  } else if (message.find("Exception when execute") != std::string_view::npos) {
    record.event_kind = "workflow_failure";
    record.failure = true;
  }

  if (message.find("Receive response in ") != std::string_view::npos) {
    record.event_kind = "http_response";
    record.endpoint = "http";
    record.latency_ms = number_after(message, "Receive response in ");
    const auto status = token_after(message, "code:");
    if (!status.empty()) record.attributes["http_status"] = status;
  } else if (message.find("send ") != std::string_view::npos &&
             message.find(" query, url:") != std::string_view::npos) {
    record.event_kind = "http_request";
    record.endpoint = "http";
  } else if (message.find("==>  Preparing:") != std::string_view::npos ||
             message.find("==> Parameters:") != std::string_view::npos ||
             message.find("<==      Total:") != std::string_view::npos) {
    record.event_kind = "sql";
    record.endpoint = "sql";
  }
}

bool decode_application_event(const EventSpan& event, InputFormat format,
                              LogRecord& record) {
  record.offset = event.offset;
  auto header = event.header;
  std::size_t outer_cursor = 0;
  const auto outer_level = take_token(header, outer_cursor);
  const auto outer_date = take_token(header, outer_cursor);
  const auto outer_time = take_token(header, outer_cursor);
  if (!is_level(outer_level) || outer_date.empty() || outer_time.empty()) return false;

  record.severity = std::string(outer_level);
  record.timestamp = normalize_app_timestamp(outer_date, outer_time);

  auto inner = header;
  if (format == InputFormat::app_export) {
    const auto outer_source = take_token(header, outer_cursor);
    (void)take_token(header, outer_cursor);
    const auto outer_service = take_token(header, outer_cursor);
    const auto outer_trace = take_token(header, outer_cursor);
    record.source = std::string(outer_source);
    record.service = std::string(outer_service);
    record.trace_id = std::string(outer_trace);
    const auto message = header.find("_msg=");
    if (message == std::string_view::npos) return false;
    inner = header.substr(message + 5);
  }

  std::size_t cursor = 0;
  const auto inner_level = take_token(inner, cursor);
  const auto inner_date = take_token(inner, cursor);
  const auto inner_time = take_token(inner, cursor);
  if (!is_level(inner_level) || inner_date.empty() || inner_time.empty()) return false;
  record.severity = std::string(inner_level);
  record.timestamp = normalize_app_timestamp(inner_date, inner_time);

  const auto inner_trace = take_bracket(inner, cursor);
  const auto pair = take_bracket(inner, cursor);
  if (!inner_trace.empty()) record.trace_id = std::string(inner_trace);
  const auto pair_colon = pair.find(':');
  if (pair_colon != std::string_view::npos) {
    record.attributes["flow_id"] = std::string(pair.substr(0, pair_colon));
    record.attributes["task_instance_id"] = std::string(pair.substr(pair_colon + 1));
  }

  (void)take_token(inner, cursor);
  const auto service = take_token(inner, cursor);
  if (!service.empty()) record.service = std::string(service);

  const auto thread_start = inner.find('[', cursor);
  if (thread_start == std::string_view::npos) return false;
  cursor = thread_start;
  record.thread = std::string(take_bracket(inner, cursor));
  record.logger = std::string(take_token(inner, cursor));
  const auto source_line = take_token(inner, cursor);
  if (record.source.empty() && !record.logger.empty())
    record.source = record.logger + ":" + std::string(source_line);
  record.message = std::string(trim_view(inner.substr(cursor)));
  record.stack_trace = continuation_text(event.continuation);
  record.exception = has_exception_line(record.stack_trace) ||
                     contains_case_insensitive(record.message, "exception");

  const auto level = lower(record.severity);
  record.failure = level == "error" || level == "fatal" || level == "critical";
  record.event_kind = record.failure ? "failure" : "log";
  record.endpoint = record.logger.empty() ? "<unknown>" : record.logger;
  enrich_application_record(record);
  return true;
}

bool spark_failure_message(std::string_view message) {
  if (contains_case_insensitive(message, "please ignore this log")) return false;
  return message.find("Final app status: FAILED") != std::string_view::npos ||
         message.find("Job aborted") != std::string_view::npos ||
         message.find("Stage failed") != std::string_view::npos ||
         message.find("Task failed") != std::string_view::npos ||
         message.find("FetchFailed") != std::string_view::npos ||
         message.find("ExecutorLostFailure") != std::string_view::npos ||
         message.find("OutOfMemoryError") != std::string_view::npos ||
         message.find("Container killed") != std::string_view::npos;
}

bool spark_advisory(std::string_view message) {
  return contains_case_insensitive(message, "deprecated") ||
         contains_case_insensitive(message, "please ignore this log") ||
         contains_case_insensitive(message, "does not exist") ||
         contains_case_insensitive(message, "skip ") ||
         contains_case_insensitive(message, "could not find proxy-user cookie") ||
         contains_case_insensitive(message, "not measuring processing time");
}

void enrich_spark_record(LogRecord& record) {
  const auto message = std::string_view(record.message);
  record.endpoint = record.logger.empty() ? "spark" : record.logger;
  const auto level = lower(record.severity);
  record.failure = level == "error" || level == "fatal" || spark_failure_message(message);
  record.advisory = level == "warn" && spark_advisory(message);
  record.event_kind = record.failure ? "failure" : "log";

  if (message.find("Final app status:") != std::string_view::npos) {
    record.event_kind = "application_outcome";
    const auto status = token_after(message, "Final app status:");
    record.attributes["application_status"] = status;
    record.attributes["exit_code"] = token_after(message, "exitCode:");
    if (status == "SUCCEEDED") record.terminal_success = true;
    if (status == "FAILED" || status == "KILLED") {
      record.terminal_failure = true;
      record.failure = true;
    }
  } else if (message.find("Job ") != std::string_view::npos &&
             message.find(" finished:") != std::string_view::npos) {
    record.event_kind = "job_completed";
    record.attributes["job_id"] = std::to_string(unsigned_after(message, "Job "));
  } else if ((message.find("ResultStage ") != std::string_view::npos ||
              message.find("ShuffleMapStage ") != std::string_view::npos) &&
             message.find(" finished in ") != std::string_view::npos) {
    record.event_kind = "stage_completed";
    const auto marker = message.find("ResultStage ") != std::string_view::npos
                            ? "ResultStage "
                            : "ShuffleMapStage ";
    record.attributes["stage_id"] = std::to_string(unsigned_after(message, marker));
  } else if (message.find("Finished task ") != std::string_view::npos) {
    record.event_kind = "task_completed";
    record.attributes["stage_id"] =
        std::to_string(unsigned_after(message, " in stage "));
    record.attributes["executor_id"] = token_after(message, "executor ");
  } else if (record.failure && message.find("Job") != std::string_view::npos) {
    record.event_kind = "job_failed";
  } else if (record.failure && message.find("Stage") != std::string_view::npos) {
    record.event_kind = "stage_failed";
  } else if (record.failure && message.find("Task") != std::string_view::npos) {
    record.event_kind = "task_failed";
  }

  double latency = number_after(message, "elapsed time:");
  if (latency < 0.0) latency = number_after(message, "finished in ");
  if (latency < 0.0) {
    const auto seconds = number_after(message, "took ");
    if (seconds >= 0.0) latency = seconds * 1000.0;
  }
  if (latency >= 0.0 && std::isfinite(latency)) record.latency_ms = latency;
}

std::string flink_job_state(std::string_view message) {
  std::string state;
  if (const auto marker = message.find("globally terminal state ");
      marker != std::string_view::npos) {
    state = token_after(message.substr(marker), "globally terminal state ");
  } else if (const auto marker = message.find("switched from state ");
             marker != std::string_view::npos) {
    const auto destination = message.find(" to ", marker);
    if (destination != std::string_view::npos)
      state = token_after(message.substr(destination), " to ");
  }
  while (!state.empty() &&
         (state.back() == '.' || state.back() == ',' || state.back() == ';' ||
          state.back() == ':'))
    state.pop_back();
  return state;
}

std::string flink_retry_operation(std::string_view message) {
  const auto marker = message.find("invoking ");
  if (marker == std::string_view::npos) return {};
  auto operation = token_after(message.substr(marker), "invoking ");
  if (operation != "class") return operation;

  const auto qualified_start = marker + std::string_view("invoking class ").size();
  const auto qualified_end = message.find(" over ", qualified_start);
  if (qualified_end == std::string_view::npos) return {};
  const auto qualified =
      message.substr(qualified_start, qualified_end - qualified_start);
  const auto dot = qualified.rfind('.');
  return std::string(dot == std::string_view::npos ? qualified
                                                   : qualified.substr(dot + 1));
}

void enrich_flink_record(LogRecord& record) {
  const auto message = std::string_view(record.message);
  const auto level = lower(record.severity);
  record.failure = level == "error" || level == "fatal" || level == "critical";
  record.event_kind = record.failure ? "failure" : "log";

  if (message.find(" fail over attempts") != std::string_view::npos ||
      message.find("failovers (") != std::string_view::npos) {
    record.retry_round = unsigned_after(message, "after ");
    if (record.retry_round == 0)
      record.retry_round = unsigned_after(message, "failovers (");
    if (record.retry_round > 0) {
      record.event_kind = "retry_attempt";
      const auto operation = flink_retry_operation(message);
      if (!operation.empty()) record.attributes["operation"] = operation;
    }
  }
  if (message.find("Not retrying because failovers") != std::string_view::npos &&
      message.find("exceeded maximum allowed") != std::string_view::npos)
    record.failure = true;

  auto job_id = token_after(message, "Job ");
  while (!job_id.empty() &&
         (job_id.back() == '.' || job_id.back() == ',' || job_id.back() == ':' ||
          job_id.back() == ')' || job_id.back() == ']'))
    job_id.pop_back();
  if (!job_id.empty()) record.attributes["job_id"] = std::move(job_id);

  const auto state = flink_job_state(message);
  if (!state.empty()) {
    record.attributes["state"] = state;
    if (state == "FINISHED") {
      record.event_kind = "job_completed";
      record.terminal_success = true;
    } else if (state == "FAILED" || state == "CANCELED") {
      record.event_kind = "job_failed";
      record.failure = true;
      record.terminal_failure = true;
    }
  } else if (record.failure && message.find("Job ") != std::string_view::npos) {
    record.event_kind = "job_failed";
  }
}

DecodedInput decode_framed(std::string_view content, std::size_t base_offset,
                           InputFormat format, const AnalyzeOptions& options) {
  DecodedInput decoded;
  decoded.format = format;
  decoded.format_name = std::string(input_format_name(format));

  if (format == InputFormat::jsonl) {
    decode_jsonl(content, base_offset, options, decoded);
  } else if (format == InputFormat::app_export || format == InputFormat::logback) {
    const auto spans = frame_events(
        content, base_offset,
        [](std::string_view line) { return looks_like_app_header(line); }, decoded);
    decoded.records.reserve(spans.size());
    for (const auto& span : spans) {
      LogRecord record;
      if (decode_application_event(span, format, record))
        decoded.records.push_back(std::move(record));
      else
        ++decoded.malformed;
    }
  } else if (format == InputFormat::spark_syslog) {
    const auto spans = frame_events(
        content, base_offset,
        [&](std::string_view line) {
          return parse_spark_header(line, options.default_timezone, nullptr);
        },
        decoded);
    decoded.records.reserve(spans.size());
    std::string application_id;
    for (const auto& span : spans) {
      LogRecord record;
      record.offset = span.offset;
      if (!parse_spark_header(span.header, options.default_timezone, &record)) {
        ++decoded.malformed;
        continue;
      }
      record.stack_trace = continuation_text(span.continuation);
      record.exception = has_exception_line(record.stack_trace);
      auto found_application = extract_identifier(record.message, "application_");
      if (found_application.empty())
        found_application = extract_identifier(record.stack_trace, "application_");
      if (!found_application.empty()) application_id = std::move(found_application);
      record.trace_id = application_id;
      if (!application_id.empty()) record.attributes["application_id"] = application_id;
      enrich_spark_record(record);
      decoded.records.push_back(std::move(record));
    }
  } else if (format == InputFormat::flink_console) {
    const auto spans = frame_events(
        content, base_offset,
        [&](std::string_view line) {
          return parse_flink_header(line, options.default_timezone, nullptr);
        },
        decoded);
    decoded.records.reserve(spans.size());
    for (const auto& span : spans) {
      LogRecord record;
      record.offset = span.offset;
      if (!parse_flink_header(span.header, options.default_timezone, &record)) {
        ++decoded.malformed;
        continue;
      }
      record.stack_trace = continuation_text(span.continuation);
      record.exception = has_exception_line(record.stack_trace) ||
                         contains_case_insensitive(record.message, "exception");
      enrich_flink_record(record);
      decoded.records.push_back(std::move(record));
    }
  }

  if (decoded.records.size() >= 2)
    decoded.reverse_order =
        decoded.records.front().timestamp > decoded.records.back().timestamp;
  return decoded;
}

}  // namespace

std::string_view input_format_name(InputFormat format) {
  switch (format) {
    case InputFormat::auto_detect:
      return "auto";
    case InputFormat::jsonl:
      return "jsonl";
    case InputFormat::app_export:
      return "app-export";
    case InputFormat::logback:
      return "logback";
    case InputFormat::spark_syslog:
      return "spark-syslog";
    case InputFormat::flink_console:
      return "flink-console";
  }
  return "unknown";
}

DecodedInput decode_input(std::string_view input, const AnalyzeOptions& options) {
  const auto container = unwrap_container(input);
  const auto format = detect_format(container.content, options.input_format);
  auto decoded =
      decode_framed(container.content, container.base_offset, format, options);
  decoded.container_name = container.name;
  return decoded;
}

}  // namespace logscope::detail
