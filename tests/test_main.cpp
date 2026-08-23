#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include "logscope/analyzer.h"
#include "logscope/output.h"

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::filesystem::path fixture(std::string_view name) {
  return std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
}

}  // namespace

int main() {
  using namespace logscope;

  const auto normalized_a = normalize_error("query timeout after 1200 ms for order 9812");
  const auto normalized_b = normalize_error("query timeout after 1350 ms for order 10421");
  expect(normalized_a == normalized_b, "dynamic numbers should normalize to the same error");

  const auto root_a = normalize_error("outer", "RuntimeError\nCaused by: Connection failed at 0x1234");
  const auto root_b = normalize_error("different outer", "RuntimeError\nCaused by: Connection failed at 0xabcd");
  expect(root_a == root_b, "stack root causes and addresses should normalize");

  const auto percentiles = calculate_percentiles({10, 20, 30, 40, 50});
  expect(percentiles.count == 5, "percentile sample count");
  expect(std::abs(percentiles.p50 - 30.0) < 0.001, "p50 interpolation");
  expect(std::abs(percentiles.p95 - 48.0) < 0.001, "p95 interpolation");

  const auto path = std::filesystem::temp_directory_path() / "logscope-test.jsonl";
  {
    std::ofstream file(path);
    file << R"({"timestamp":"2026-01-01T00:00:00Z","level":"INFO","service":"api","trace_id":"t1","endpoint":"GET /a","latency_ms":10,"message":"ok"})" << '\n';
    file << "broken json\n";
    file << R"({"timestamp":"2026-01-01T00:01:00Z","level":"ERROR","service":"db","trace_id":"t2","endpoint":"GET /a","latency_ms":100,"message":"query timeout 123"})" << '\n';
  }
  AnalyzeOptions options;
  const auto result = analyze_file(path, options);
  expect(result.lines == 3, "all physical lines should be counted");
  expect(result.events == 3, "all JSONL records should be counted as events");
  expect(result.detected_format == "jsonl", "JSONL should be auto-detected");
  expect(result.malformed == 1, "malformed lines should be counted");
  expect(result.matched == 2, "valid matching records should be counted");
  expect(result.errors.size() == 1, "error should be grouped");
  expect(result.endpoint_latencies.at("GET /a").size() == 2, "latencies should be retained");

  AnalyzeOptions filtered;
  filtered.filters.service = "db";
  const auto filtered_result = analyze_file(path, filtered);
  expect(filtered_result.matched == 1, "service filter should select one record");
  std::filesystem::remove(path);

  AnalyzeOptions app_options;
  app_options.retain_records = true;
  const auto app_result = analyze_file(fixture("app_export.log"), app_options);
  expect(app_result.detected_format == "app-export",
         "application export should be auto-detected");
  expect(app_result.events == 5, "application export should frame logical events");
  expect(app_result.continuations == 4,
         "exception declarations and frames should be continuations");
  expect(app_result.malformed == 0, "sanitized application export should parse");
  expect(app_result.outcome == "failed", "terminal workflow state should win");
  expect(app_result.error_events == 3,
         "two exceptions and terminal state should be failures");
  expect(app_result.exception_events == 2, "multiline exceptions should be attached");
  expect(app_result.retries == 1, "round two should imply one retry");
  expect(app_result.states.at("Failed") == 1, "terminal state should be extracted");
  expect(app_result.records.front().trace_id == "trace-a",
         "reverse-ordered exports should be sorted chronologically when retained");
  expect(app_result.records.front().attributes.at("workflow_id") == "100",
         "workflow identifiers should be extracted");
  AnalyzeOptions app_filtered;
  app_filtered.filters.level = "INFO";
  expect(analyze_file(fixture("app_export.log"), app_filtered).outcome == "failed",
         "filters should not hide the source-wide terminal failure");

  AnalyzeOptions spark_options;
  spark_options.retain_records = true;
  spark_options.default_timezone = "+00:00";
  const auto spark_result = analyze_file(fixture("spark_syslog.html"), spark_options);
  expect(spark_result.source_file == "spark_syslog.html",
         "analysis should retain the scanned file name");
  expect(spark_result.detected_format == "spark-syslog",
         "Spark syslog should be auto-detected inside HTML");
  expect(spark_result.container_format == "html-xmp",
         "browser wrapper should be reported");
  expect(spark_result.events == 7, "Spark records should be framed");
  expect(spark_result.continuations == 2, "Spark stack trace should be attached");
  expect(spark_result.outcome == "succeeded",
         "terminal Spark status should override warning-shaped noise");
  expect(spark_result.warnings == 1, "Spark warning should be counted");
  expect(spark_result.advisory_events == 1,
         "explicitly ignorable warning should be advisory");
  expect(spark_result.exception_events == 1,
         "incidental Spark exception should remain visible");
  expect(spark_result.error_events == 0,
         "incidental exception should not imply application failure");
  expect(spark_result.completed_jobs == 1, "Spark job completion should be extracted");
  expect(spark_result.completed_stages == 1,
         "Spark stage completion should be extracted");
  expect(spark_result.completed_tasks == 1,
         "Spark task completion should be extracted");
  const auto spaced_thread = std::find_if(
      spark_result.records.begin(), spark_result.records.end(),
      [](const LogRecord& record) {
        return record.thread.find("diagnostic thread name") != std::string::npos;
      });
  expect(spaced_thread != spark_result.records.end(),
         "Spark thread names containing spaces should parse");
  AnalyzeOptions spark_filtered;
  spark_filtered.filters.level = "WARN";
  expect(analyze_file(fixture("spark_syslog.html"), spark_filtered).outcome ==
             "succeeded",
         "filters should not hide the source-wide Spark success");

  std::ostringstream json;
  RenderOptions render;
  render.format = OutputFormat::json;
  render_analysis(json, spark_result, render);
  expect(json.str().find("\"file\": \"spark_syslog.html\"") != std::string::npos,
         "JSON output should contain the scanned file name");
  expect(json.str().find("\"outcome\": \"succeeded\"") != std::string::npos,
         "JSON output should contain the resolved outcome");

  if (failures == 0) std::cout << "All tests passed\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
