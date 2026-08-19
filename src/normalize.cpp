#include "logscope/analyzer.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

namespace logscope {
namespace {

std::string root_cause_line(std::string_view message, std::string_view stack) {
  std::string candidate(message);
  std::size_t position = 0;
  while (position < stack.size()) {
    const auto end = stack.find('\n', position);
    auto line = stack.substr(position, end == std::string_view::npos ? stack.size() - position
                                                                    : end - position);
    const auto caused = line.find("Caused by:");
    if (caused != std::string_view::npos) candidate.assign(line.substr(caused + 10));
    position = end == std::string_view::npos ? stack.size() : end + 1;
  }
  return candidate;
}

}  // namespace

std::string normalize_error(std::string_view message, std::string_view stack_trace) {
  auto value = root_cause_line(message, stack_trace);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

  static const std::regex uuid(
      R"(\b[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\b)",
      std::regex::icase);
  static const std::regex hex_address(R"(\b0x[0-9a-f]+\b)", std::regex::icase);
  static const std::regex ipv4(R"(\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b)");
  static const std::regex number(R"(\b[0-9]+(?:\.[0-9]+)?\b)");
  static const std::regex whitespace(R"(\s+)");

  value = std::regex_replace(value, uuid, "<uuid>");
  value = std::regex_replace(value, hex_address, "<addr>");
  value = std::regex_replace(value, ipv4, "<ip>");
  value = std::regex_replace(value, number, "<n>");
  value = std::regex_replace(value, whitespace, " ");
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
  return value.empty() ? "<empty error>" : value;
}

Percentiles calculate_percentiles(std::vector<double> values) {
  Percentiles result;
  result.count = values.size();
  if (values.empty()) return result;
  std::sort(values.begin(), values.end());
  const auto percentile = [&](double p) {
    const double index = p * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(index);
    const auto upper = std::min(lower + 1, values.size() - 1);
    const double fraction = index - static_cast<double>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
  };
  result.p50 = percentile(0.50);
  result.p95 = percentile(0.95);
  result.p99 = percentile(0.99);
  result.max = values.back();
  return result;
}

}  // namespace logscope
