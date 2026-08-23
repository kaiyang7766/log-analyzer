#include "logscope/analyzer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace logscope {
namespace {

std::string root_cause_line(std::string_view message, std::string_view stack) {
  std::string candidate(message);
  std::string first_frame;
  std::size_t position = 0;
  while (position < stack.size()) {
    const auto end = stack.find('\n', position);
    auto line = stack.substr(position, end == std::string_view::npos ? stack.size() - position
                                                                    : end - position);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
      line.remove_prefix(1);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
      line.remove_suffix(1);
    const auto caused = line.find("Caused by:");
    if (caused != std::string_view::npos) {
      candidate.assign(line.substr(caused + 10));
    } else {
      const auto colon = line.find(':');
      const auto type = colon == std::string_view::npos ? line : line.substr(0, colon);
      if (type.ends_with("Exception") || type.ends_with("Error"))
        candidate.assign(line);
    }
    if (first_frame.empty() && line.starts_with("at "))
      first_frame.assign(line.substr(3));
    position = end == std::string_view::npos ? stack.size() : end + 1;
  }
  if (!first_frame.empty()) candidate += " @ " + first_frame;
  return candidate;
}

bool is_word_character(unsigned char ch) {
  return std::isalnum(ch) || ch == '_';
}

bool has_word_start(std::string_view value, std::size_t position) {
  return position == 0 ||
         !is_word_character(static_cast<unsigned char>(value[position - 1]));
}

bool has_word_end(std::string_view value, std::size_t position) {
  return position == value.size() ||
         !is_word_character(static_cast<unsigned char>(value[position]));
}

bool is_hexadecimal(unsigned char ch) {
  return std::isdigit(ch) || (ch >= 'a' && ch <= 'f');
}

std::size_t uuid_length_at(std::string_view value, std::size_t position) {
  static constexpr std::array<std::size_t, 4> hyphens{8, 13, 18, 23};
  constexpr std::size_t length = 36;
  if (!has_word_start(value, position) || position + length > value.size())
    return 0;
  for (std::size_t index = 0; index < length; ++index) {
    if (std::find(hyphens.begin(), hyphens.end(), index) != hyphens.end()) {
      if (value[position + index] != '-') return 0;
    } else if (!is_hexadecimal(
                   static_cast<unsigned char>(value[position + index]))) {
      return 0;
    }
  }
  const auto version = value[position + 14];
  const auto variant = value[position + 19];
  if (version < '1' || version > '5' ||
      (variant != '8' && variant != '9' && variant != 'a' &&
       variant != 'b'))
    return 0;
  return has_word_end(value, position + length) ? length : 0;
}

std::size_t hex_address_length_at(std::string_view value,
                                  std::size_t position) {
  if (!has_word_start(value, position) || position + 2 >= value.size() ||
      value[position] != '0' || value[position + 1] != 'x')
    return 0;
  auto end = position + 2;
  while (end < value.size() &&
         is_hexadecimal(static_cast<unsigned char>(value[end])))
    ++end;
  if (end == position + 2 || !has_word_end(value, end)) return 0;
  return end - position;
}

std::size_t ipv4_length_at(std::string_view value, std::size_t position) {
  if (!has_word_start(value, position)) return 0;
  auto cursor = position;
  for (int segment = 0; segment < 4; ++segment) {
    const auto start = cursor;
    while (cursor < value.size() && cursor - start < 3 &&
           std::isdigit(static_cast<unsigned char>(value[cursor])))
      ++cursor;
    if (cursor == start) return 0;
    if (segment < 3) {
      if (cursor >= value.size() || value[cursor] != '.') return 0;
      ++cursor;
    }
  }
  return has_word_end(value, cursor) ? cursor - position : 0;
}

std::size_t number_length_at(std::string_view value, std::size_t position) {
  if (!has_word_start(value, position) ||
      !std::isdigit(static_cast<unsigned char>(value[position])))
    return 0;
  auto cursor = position;
  while (cursor < value.size() &&
         std::isdigit(static_cast<unsigned char>(value[cursor])))
    ++cursor;
  if (cursor + 1 < value.size() && value[cursor] == '.' &&
      std::isdigit(static_cast<unsigned char>(value[cursor + 1]))) {
    ++cursor;
    while (cursor < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[cursor])))
      ++cursor;
  }
  return has_word_end(value, cursor) ? cursor - position : 0;
}

std::string normalize_dynamic_values(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });

  std::string normalized;
  normalized.reserve(value.size());
  for (std::size_t cursor = 0; cursor < value.size();) {
    const auto append_replacement =
        [&](std::size_t length, std::string_view replacement) {
          normalized.append(replacement);
          cursor += length;
        };

    if (const auto length = uuid_length_at(value, cursor); length > 0) {
      append_replacement(length, "<uuid>");
    } else if (const auto length = hex_address_length_at(value, cursor);
               length > 0) {
      append_replacement(length, "<addr>");
    } else if (const auto length = ipv4_length_at(value, cursor); length > 0) {
      append_replacement(length, "<ip>");
    } else if (const auto length = number_length_at(value, cursor);
               length > 0) {
      append_replacement(length, "<n>");
    } else if (std::isspace(static_cast<unsigned char>(value[cursor]))) {
      if (!normalized.empty() && normalized.back() != ' ')
        normalized.push_back(' ');
      ++cursor;
    } else {
      normalized.push_back(value[cursor]);
      ++cursor;
    }
  }
  if (!normalized.empty() && normalized.back() == ' ')
    normalized.pop_back();
  return normalized;
}

}  // namespace

std::string normalize_error(std::string_view message, std::string_view stack_trace) {
  auto value = normalize_dynamic_values(
      root_cause_line(message, stack_trace));
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
