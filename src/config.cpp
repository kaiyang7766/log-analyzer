#include "logscope/config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace logscope {
namespace {

std::string trim(std::string value) {
  const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

void assign(FieldConfig& config, std::string_view key, std::string value) {
  if (key == "timestamp") config.timestamp = std::move(value);
  else if (key == "severity") config.severity = std::move(value);
  else if (key == "service") config.service = std::move(value);
  else if (key == "trace_id") config.trace_id = std::move(value);
  else if (key == "latency_ms") config.latency_ms = std::move(value);
  else if (key == "message") config.message = std::move(value);
  else if (key == "endpoint") config.endpoint = std::move(value);
  else if (key == "stack_trace") config.stack_trace = std::move(value);
}

}  // namespace

FieldConfig load_config(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open config: " + path.string());

  FieldConfig config;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const auto comment = line.find('#');
    if (comment != std::string::npos) line.erase(comment);
    line = trim(std::move(line));
    if (line.empty()) continue;
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      throw std::runtime_error("invalid config line " + std::to_string(line_number));
    }
    auto key = trim(line.substr(0, colon));
    auto value = trim(line.substr(colon + 1));
    if (value.size() >= 2 && ((value.front() == '\"' && value.back() == '\"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }
    if (key.empty() || value.empty()) {
      throw std::runtime_error("empty config key or value on line " + std::to_string(line_number));
    }
    assign(config, key, std::move(value));
  }
  return config;
}

std::string default_config_yaml() {
  return "timestamp: timestamp\nseverity: level\nservice: service\ntrace_id: trace_id\n"
         "latency_ms: latency_ms\nmessage: message\nendpoint: endpoint\nstack_trace: stack_trace\n";
}

}  // namespace logscope
