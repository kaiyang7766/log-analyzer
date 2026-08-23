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

constexpr std::string_view usage = R"(LogScope++ 0.2.0

Usage:
  logscope analyze FILE [FILTERS] [--format text|markdown|json]
  logscope errors  FILE [FILTERS] [--group-by error|service|endpoint]
  logscope latency FILE [FILTERS] [--group-by endpoint|service]

Filters:
  --from TIMESTAMP       Include normalized timestamps >= this value
  --to TIMESTAMP         Include normalized timestamps <= this value
  --level LEVEL          Match a severity exactly (case-insensitive)
  --service SERVICE      Match a service exactly
  --trace-id ID          Match a trace ID exactly
  --text TEXT            Search message and stack trace (case-insensitive)

Input:
  --input-format FORMAT  auto|jsonl|app-export|logback|spark-syslog|flink-console
  --timezone SUFFIX      Append an offset to Spark/Flink timestamps, for example +08:00
  --io buffered|mmap     Select the input ownership strategy

Notes:
  Auto detection supports JSONL, enriched application exports, raw Logback,
  Spark syslog, Flink console logs, and Spark syslog inside an HTML xmp element.
  "recent" compares chronological halves, including reverse-ordered exports.
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

logscope::InputFormat parse_input_format(std::string_view value) {
  if (value == "auto") return logscope::InputFormat::auto_detect;
  if (value == "jsonl") return logscope::InputFormat::jsonl;
  if (value == "app-export") return logscope::InputFormat::app_export;
  if (value == "logback") return logscope::InputFormat::logback;
  if (value == "spark-syslog") return logscope::InputFormat::spark_syslog;
  if (value == "flink-console") return logscope::InputFormat::flink_console;
  throw std::runtime_error(
      "--input-format must be auto, jsonl, app-export, logback, spark-syslog, "
      "or flink-console");
}

Cli parse_cli(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
    std::cout << usage;
    std::exit(0);
  }
  if (std::string_view(argv[1]) == "--version") {
    std::cout << "logscope 0.2.0\n";
    std::exit(0);
  }
  if (argc < 3) throw std::runtime_error("a command and input file are required");

  Cli cli;
  cli.command = argv[1];
  cli.file = argv[2];
  if (cli.command != "analyze" && cli.command != "errors" && cli.command != "latency")
    throw std::runtime_error("unknown command: " + cli.command);
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
    else if (option == "--input-format")
      cli.analysis.input_format =
          parse_input_format(require_value(argc, argv, index, option));
    else if (option == "--timezone")
      cli.analysis.default_timezone = require_value(argc, argv, index, option);
    else if (option == "--io") {
      const auto value = require_value(argc, argv, index, option);
      if (value == "buffered") cli.analysis.io_mode = logscope::IoMode::buffered;
      else if (value == "mmap") cli.analysis.io_mode = logscope::IoMode::mmap;
      else throw std::runtime_error("--io must be buffered or mmap");
    }
    else if (option == "--format") {
      const auto value = require_value(argc, argv, index, option);
      if (value == "text") cli.render.format = logscope::OutputFormat::text;
      else if (value == "markdown") cli.render.format = logscope::OutputFormat::markdown;
      else if (value == "json") cli.render.format = logscope::OutputFormat::json;
      else throw std::runtime_error("--format must be text, markdown, or json");
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
