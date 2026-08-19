#include <charconv>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "logscope/analyzer.h"
#include "logscope/output.h"

namespace {

constexpr std::string_view usage = R"(LogScope++ 0.1.0 (Day 1)

Usage:
  logscope analyze FILE [FILTERS] [--limit N] [--format text|markdown]
  logscope errors  FILE [FILTERS] [--group-by error|service|endpoint] [--limit N]
  logscope latency FILE [FILTERS] [--group-by endpoint|service] [--limit N]

Filters:
  --from TIMESTAMP       Include timestamps >= this ISO-8601 value
  --to TIMESTAMP         Include timestamps <= this ISO-8601 value
  --level LEVEL          Match a severity exactly (case-insensitive)
  --service SERVICE      Match a service exactly
  --trace-id ID          Match a trace ID exactly
  --text TEXT            Search message and stack trace (case-insensitive)

Expected JSON fields:
  timestamp, level, service, trace_id, latency_ms, message, endpoint, stack_trace

Notes:
  "recent" compares the second half of the input with the first half.
  Malformed JSON lines are counted and skipped.
)";

struct Cli {
  std::string command;
  std::filesystem::path file;
  logscope::AnalyzeOptions analysis;
  logscope::RenderOptions render;
};

std::string require_value(int argc, char** argv, int& index, std::string_view option) {
  if (index + 1 >= argc) throw std::runtime_error("missing value for " + std::string(option));
  return argv[++index];
}

std::size_t parse_size(std::string_view value, std::string_view option) {
  std::size_t parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size())
    throw std::runtime_error("invalid number for " + std::string(option));
  return parsed;
}

Cli parse_cli(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
    std::cout << usage;
    std::exit(0);
  }
  if (std::string_view(argv[1]) == "--version") {
    std::cout << "logscope 0.1.0\n";
    std::exit(0);
  }
  if (argc < 3) throw std::runtime_error("a command and input file are required");

  Cli cli;
  cli.command = argv[1];
  cli.file = argv[2];
  if (cli.command != "analyze" && cli.command != "errors" && cli.command != "latency")
    throw std::runtime_error("unknown Day 1 command: " + cli.command);
  cli.render.group_by = cli.command == "errors" ? "error" : "endpoint";

  for (int index = 3; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (option == "--from") cli.analysis.filters.from = require_value(argc, argv, index, option);
    else if (option == "--to") cli.analysis.filters.to = require_value(argc, argv, index, option);
    else if (option == "--level") cli.analysis.filters.level = require_value(argc, argv, index, option);
    else if (option == "--service") cli.analysis.filters.service = require_value(argc, argv, index, option);
    else if (option == "--trace-id") cli.analysis.filters.trace_id = require_value(argc, argv, index, option);
    else if (option == "--text") cli.analysis.filters.text = require_value(argc, argv, index, option);
    else if (option == "--group-by") cli.render.group_by = require_value(argc, argv, index, option);
    else if (option == "--limit") cli.render.limit = parse_size(require_value(argc, argv, index, option), option);
    else if (option == "--format") {
      const auto value = require_value(argc, argv, index, option);
      if (value == "text") cli.render.format = logscope::OutputFormat::text;
      else if (value == "markdown") cli.render.format = logscope::OutputFormat::markdown;
      else throw std::runtime_error("--format must be text or markdown in the Day 1 build");
    } else if (option == "--help" || option == "-h") {
      std::cout << usage;
      std::exit(0);
    } else {
      throw std::runtime_error("unknown option: " + std::string(option));
    }
  }

  if (cli.command == "errors" && cli.render.group_by != "error" &&
      cli.render.group_by != "service" && cli.render.group_by != "endpoint")
    throw std::runtime_error("errors --group-by must be error, service, or endpoint");
  if (cli.command == "latency" && cli.render.group_by != "endpoint" &&
      cli.render.group_by != "service")
    throw std::runtime_error("latency --group-by must be endpoint or service");
  return cli;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto cli = parse_cli(argc, argv);
    const auto result = logscope::analyze_file(cli.file, cli.analysis);
    if (cli.command == "analyze") logscope::render_analysis(std::cout, result, cli.render);
    else if (cli.command == "errors") logscope::render_errors(std::cout, result, cli.render);
    else logscope::render_latency(std::cout, result, cli.render);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "logscope: " << error.what() << "\nRun 'logscope --help' for usage.\n";
    return 2;
  }
}
