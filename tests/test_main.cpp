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

  AnalyzeOptions flink_options;
  flink_options.retain_records = true;
  flink_options.default_timezone = "+08:00";
  const auto flink_result =
      analyze_file(fixture("flink_jobmanager.out"), flink_options);
  expect(flink_result.detected_format == "flink-console",
         "Flink console logs should be auto-detected");
  expect(flink_result.events == 4, "Flink records should be framed");
  expect(flink_result.continuations == 2,
         "Flink stack traces should be attached");
  expect(flink_result.malformed == 1,
         "unstructured launcher preamble should be counted");
  expect(flink_result.records.front().timestamp ==
             "2026-08-14T16:51:05+08:00",
         "Flink timestamps should be normalized");
  expect(flink_result.first_timestamp == "2026-08-14T16:51:05+08:00" &&
             flink_result.last_timestamp == "2026-08-14T16:51:08+08:00",
         "Flink analysis should report its time range");
  expect(flink_result.records.front().service == "flink",
         "Flink records should identify their service");
  expect(flink_result.warnings == 1, "Flink warnings should be counted");
  expect(flink_result.exception_events == 1,
         "Flink multiline exceptions should be recognized");
  expect(flink_result.exception_groups.size() == 1,
         "Flink exception root causes should be grouped");
  expect(flink_result.retries == 1,
         "Flink failover attempts should contribute retry counts");
  expect(flink_result.retry_operations.at("getFileInfo") == 2,
         "Flink summaries should retain maximum attempts by operation");
  expect(flink_result.completed_jobs == 1,
         "Flink FINISHED states should complete jobs");
  expect(flink_result.failed_jobs == 1,
         "Flink FAILED states should fail jobs");
  expect(flink_result.outcome == "failed",
         "a terminal Flink failure should determine the outcome");
  std::ostringstream flink_text;
  render_analysis(flink_text, flink_result, RenderOptions{});
  expect(flink_text.str().find("Exception root causes") != std::string::npos,
         "Flink summaries should include exception root causes");
  expect(flink_text.str().find("Flink components") != std::string::npos,
         "Flink summaries should include logger activity");
  expect(flink_text.str().find("Retry operations") != std::string::npos,
         "Flink summaries should include retry operations");
  expect(flink_text.str().find("Endpoint latency") == std::string::npos,
         "Flink summaries should omit empty endpoint latency");

  const auto padded_flink_path =
      std::filesystem::temp_directory_path() / "logscope-padded-jobmanager.log";
  {
    std::ofstream file(padded_flink_path, std::ios::binary);
    file.write("\0\0\0\0", 4);
    file << "2026-07-21 02:51:41,101 INFO  com.example.Service"
            "     worker-1     [] - Started service\n";
    file << " 2026-07-21 02:51:42,202 WARN  org.apache.flink.runtime.Worker"
            "     jobmanager-future-thread-1     [] - Delayed checkpoint\n";
  }
  AnalyzeOptions padded_flink_options;
  padded_flink_options.retain_records = true;
  padded_flink_options.default_timezone = "+00:00";
  const auto padded_flink_result =
      analyze_file(padded_flink_path, padded_flink_options);
  expect(padded_flink_result.detected_format == "flink-console",
         "column-aligned Flink logs should be auto-detected");
  expect(padded_flink_result.container_format == "nul-padded-text",
         "leading NUL padding should be reported as a container");
  expect(padded_flink_result.events == 2 && padded_flink_result.malformed == 0,
         "NUL-padded Flink records should parse without malformed input");
  expect(padded_flink_result.records.front().offset == 4,
         "record offsets should include stripped NUL padding");
  expect(padded_flink_result.records.front().timestamp ==
             "2026-07-21T02:51:41,101+00:00",
         "full-year Flink timestamps should be normalized");
  expect(padded_flink_result.records.front().logger == "com.example.Service" &&
             padded_flink_result.records.front().thread == "worker-1",
         "column-aligned Flink logger and thread fields should parse");
  expect(padded_flink_result.warnings == 1,
         "column-aligned Flink warning levels should be counted");
  std::filesystem::remove(padded_flink_path);

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
