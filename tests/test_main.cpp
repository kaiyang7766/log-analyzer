#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "log_analyzer/analyzer.h"
#include "log_analyzer/output.h"

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
  using namespace log_analyzer;

  const auto normalized_a = normalize_error("query timeout after 1200 ms for order 9812");
  const auto normalized_b = normalize_error("query timeout after 1350 ms for order 10421");
  expect(normalized_a == normalized_b, "dynamic numbers should normalize to the same error");

  const auto root_a = normalize_error("outer", "RuntimeError\nCaused by: Connection failed at 0x1234");
  const auto root_b = normalize_error("different outer", "RuntimeError\nCaused by: Connection failed at 0xabcd");
  expect(root_a == root_b, "stack root causes and addresses should normalize");
  expect(normalize_error(
             "Host 10.20.30.40 request "
             "550E8400-E29B-41D4-A716-446655440000 took 12.5 ms") ==
             "host <ip> request <uuid> took <n> ms",
         "IP addresses, UUIDs, and decimals should normalize");
  expect(normalize_error("  error\tfor item123 and 123item  ") ==
             "error for item123 and 123item",
         "whitespace should collapse without replacing embedded numbers");

  const auto percentiles = calculate_percentiles({10, 20, 30, 40, 50});
  expect(percentiles.count == 5, "percentile sample count");
  expect(std::abs(percentiles.p50 - 30.0) < 0.001, "p50 interpolation");
  expect(std::abs(percentiles.p95 - 48.0) < 0.001, "p95 interpolation");

  const auto path = std::filesystem::temp_directory_path() / "log-analyzer-test.jsonl";
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
      std::filesystem::temp_directory_path() / "log-analyzer-padded-jobmanager.log";
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

  const auto xmp_mention_path =
      std::filesystem::temp_directory_path() / "log-analyzer-xmp-mention.log";
  {
    std::ofstream file(xmp_mention_path, std::ios::binary);
    file << "26/08/20 07:24:02.012 Driver INFO SparkUI: Rendering <xmp> log view\n";
    file << "26/08/20 07:24:03.013 Driver INFO SparkUI: Log view rendered\n";
  }
  const auto xmp_mention_result =
      analyze_file(xmp_mention_path, AnalyzeOptions{});
  expect(xmp_mention_result.container_format == "plain-text" &&
             xmp_mention_result.events == 2,
         "a plain log that mentions <xmp> should not be unwrapped as HTML");
  std::filesystem::remove(xmp_mention_path);

  // Padding lengths around the 64-byte skip block.
  for (const std::size_t padding : {63, 64, 65, 130}) {
    const auto padded_path =
        std::filesystem::temp_directory_path() / "log-analyzer-odd-padding.log";
    {
      std::ofstream file(padded_path, std::ios::binary);
      file << std::string(padding, '\0');
      file << "26/08/20 07:24:02.012 Driver INFO SparkUI: first\n";
      file << "26/08/20 07:24:03.013 Driver INFO SparkUI: second\n";
    }
    const auto padded_result = analyze_file(padded_path, AnalyzeOptions{});
    expect(padded_result.container_format == "nul-padded-text" &&
               padded_result.events == 2 && padded_result.malformed == 0,
           "leading NUL padding of any length should be skipped exactly");
    std::filesystem::remove(padded_path);
  }

  const auto padded_halves_path =
      std::filesystem::temp_directory_path() / "log-analyzer-padded-halves.log";
  {
    std::ofstream file(padded_halves_path, std::ios::binary);
    file << std::string(4096, '\0');
    file << "2026-07-21 02:51:41,101 ERROR com.example.Sink"
            "     worker-1     [] - Sink write failed\n";
    for (int i = 0; i < 4; ++i)
      file << "2026-07-21 02:51:42,202 INFO  com.example.Sink"
              "     worker-1     [] - Checkpoint completed\n";
    file << "2026-07-21 02:51:43,303 WARN  com.example.HadoopRecoverableWriter"
            "     jobmanager-future-thread-3     [] - WARNING: the"
            " StreamingFileSink will throw an exception upon recovery.\n";
    file << "2026-07-21 02:51:44,404 WARN  com.example.Client"
            "     worker-1     [] - java.io.IOException: storage unavailable\n";
  }
  const auto padded_halves_result =
      analyze_file(padded_halves_path, AnalyzeOptions{});
  expect(padded_halves_result.errors.size() == 1 &&
             padded_halves_result.errors.begin()->second.previous == 1 &&
             padded_halves_result.errors.begin()->second.recent == 0,
         "padding larger than the content should not shift records into the recent half");
  expect(padded_halves_result.warnings == 2,
         "both Flink warnings should be counted");
  expect(padded_halves_result.exception_events == 1,
         "prose mentioning an exception should not count as an exception event");
  expect(padded_halves_result.exception_groups.size() == 1 &&
             padded_halves_result.exception_groups.begin()->first.find(
                 "java.io.ioexception") != std::string::npos,
         "a logged exception type in the message should still count");
  std::filesystem::remove(padded_halves_path);

  AnalyzeOptions scheduler_options;
  scheduler_options.retain_records = true;
  scheduler_options.default_timezone = "+08:00";
  const auto scheduler_result =
      analyze_file(fixture("scheduler_instance.log"), scheduler_options);
  expect(scheduler_result.detected_format == "scheduler",
         "scheduler instance logs should be auto-detected");
  expect(scheduler_result.lines == 34 && scheduler_result.events == 22,
         "scheduler and embedded task headers should both frame events");
  expect(scheduler_result.continuations == 10 && scheduler_result.malformed == 0,
         "task stdout and stack traces should be continuation lines");
  expect(scheduler_result.outcome == "failed",
         "a terminal scheduler failure should determine the outcome");
  expect(scheduler_result.outcome_evidence ==
             "Exit status:FAILED (exit code 1; retries used: 3 of 3, exhausted)",
         "scheduler evidence should include exit code and retry budget");
  expect(scheduler_result.error_events == 4,
         "task failures, scheduler errors, and the failed transition should count once each");
  expect(scheduler_result.warnings == 3 && scheduler_result.advisory_events == 3,
         "agent noise and deprecations should be advisory warnings");
  expect(scheduler_result.exception_events == 2,
         "agent noise and metric names should not count as exceptions");
  const auto pool_root_cause = std::find_if(
      scheduler_result.exception_groups.begin(),
      scheduler_result.exception_groups.end(), [](const auto& group) {
        return group.first.find("poolexception: connection pool is empty") !=
               std::string::npos;
      });
  expect(pool_root_cause != scheduler_result.exception_groups.end(),
         "the task stack trace root cause should be grouped");
  expect(scheduler_result.retries == 3 &&
             scheduler_result.retry_operations.at("instance") == 4,
         "scheduler retries should count completed retries");
  expect(scheduler_result.failed_jobs == 1,
         "task-reported job failures should remain lifecycle evidence");
  const auto& scheduler = scheduler_result.scheduler;
  expect(scheduler.instance_id == "1002", "the retried instance should be identified");
  expect(scheduler.state_path ==
             std::vector<std::string>{"SUBMITTED", "EXECUTOR_PENDING", "RUNNING",
                                      "FAILED"},
         "scheduler status transitions should form a path");
  expect(scheduler.exit_code == "1" && scheduler.current_retry == 3 &&
             scheduler.max_retries == 3 && scheduler.retry_exhausted,
         "scheduler exit code and retry budget should be extracted");
  expect(scheduler.unframed_task_lines == 2,
         "stdout after lifecycle messages should not become their stack trace");
  expect(scheduler.task_events.at("log4j") == 4 &&
             scheduler.task_events.at("java-logging") == 1 &&
             scheduler.task_events.at("spark-console") == 1,
         "embedded task output formats should be counted");
  const auto record_with = [&](std::string_view message) {
    return std::find_if(scheduler_result.records.begin(),
                        scheduler_result.records.end(),
                        [&](const LogRecord& record) { return record.message == message; });
  };
  const auto java_logging = record_with("send msg in cache fail status:111");
  expect(java_logging != scheduler_result.records.end() &&
             java_logging->timestamp == "2026-05-15T00:51:46+08:00" &&
             java_logging->severity == "WARN" &&
             java_logging->logger == "org.example.shipping.Cache",
         "two-line java.util.logging records should parse");
  const auto replayed = record_with("Engine job [42], status [RUNNING]");
  expect(replayed != scheduler_result.records.end() &&
             replayed->timestamp == "2026-05-15T00:51:48.500+08:00" &&
             replayed->attributes.contains("replayed"),
         "replayed scheduler lines should keep their original header");
  const auto client = record_with("Starting command line client");
  expect(client != scheduler_result.records.end() &&
             client->timestamp == "2026-05-15T00:51:46.001+08:00" &&
             client->service == "task" &&
             client->endpoint == "org.example.client.CliFrontend",
         "Flink client lines without thread columns should parse as task output");
  expect(scheduler_result.first_timestamp == "2026-05-15T00:46:42.806+08:00" &&
             scheduler_result.last_timestamp == "2026-05-15T00:51:51.900+08:00",
         "scheduler analysis should report its time range");
  AnalyzeOptions scheduler_filtered;
  scheduler_filtered.filters.level = "WARN";
  const auto scheduler_filtered_result =
      analyze_file(fixture("scheduler_instance.log"), scheduler_filtered);
  expect(scheduler_filtered_result.outcome == "failed" &&
             scheduler_filtered_result.scheduler.state_path.size() == 4,
         "filters should not hide the scheduler outcome or status path");
  std::ostringstream scheduler_text;
  render_analysis(scheduler_text, scheduler_result, RenderOptions{});
  expect(scheduler_text.str().find(
             "Status path: SUBMITTED -> EXECUTOR_PENDING -> RUNNING -> FAILED") !=
             std::string::npos,
         "scheduler summaries should include the status path");
  std::ostringstream scheduler_json;
  RenderOptions scheduler_render;
  scheduler_render.format = OutputFormat::json;
  render_analysis(scheduler_json, scheduler_result, scheduler_render);
  expect(scheduler_json.str().find("\"retry_exhausted\": true") != std::string::npos,
         "scheduler JSON should include the retry budget");

  const auto write_scheduler_log = [](std::string_view name, std::string_view content) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary);
    file << content;
    return path;
  };
  const auto embedded_path = write_scheduler_log(
      "log-analyzer-scheduler-embedded.log",
      "2026-06-01T10:00:00.000 INFO  Instance created with id 2001\n"
      "2026-06-01T10:00:01.000 INFO  Start to change instance status from "
      "EXECUTOR_PENDING to RUNNING.\n"
      "26/06/01 10:00:02 WARN SparkConf: Example key is deprecated\n"
      "26/06/01 10:00:03 INFO Client: Submitting application\n"
      "26/06/01 10:00:04 INFO Client: Application report\n"
      "2026-06-01 10:00:05,000 INFO  org.example.flink.Dispatcher - Job def456 "
      "switched from state RUNNING to FAILED.\n"
      "2026-06-01T10:00:06.000 INFO  execute status: SUCCEEDED\n"
      "2026-06-01T10:00:06.100 INFO  Start to change instance status from RUNNING "
      "to SUCCEEDED.\n");
  const auto embedded_result = analyze_file(embedded_path, AnalyzeOptions{});
  expect(embedded_result.detected_format == "scheduler",
         "embedded Spark and Flink lines should not outvote scheduler headers");
  expect(embedded_result.outcome == "succeeded" && embedded_result.failed_jobs == 1,
         "task-reported terminal states should not override the scheduler");
  std::filesystem::remove(embedded_path);

  const auto running_path = write_scheduler_log(
      "log-analyzer-scheduler-running.log",
      "2026-06-02T09:00:00.000 INFO  Instance created with id 3001\n"
      "2026-06-02T09:00:01.000 INFO  Start to change instance status from "
      "EXECUTOR_PENDING to RUNNING.\n"
      "2026-06-02T09:00:02.000 ERROR Engine poll failed\n");
  const auto running_result = analyze_file(running_path, AnalyzeOptions{});
  expect(running_result.outcome == "unknown",
         "a running instance should not fall back to a failed outcome");
  expect(running_result.outcome_evidence ==
             "No terminal status was found; instance status has been RUNNING since "
             "2026-06-02T09:00:01.000; 1 failure events so far.",
         "running instance evidence should name the last status");
  std::filesystem::remove(running_path);

  const auto retry_path = write_scheduler_log(
      "log-analyzer-scheduler-retry.log",
      "2026-06-03T09:00:00.000 INFO  Instance created with id 4001\n"
      "2026-06-03T09:00:01.000 INFO  Start to change instance status from RUNNING "
      "to FAILED_RETRY.\n");
  const auto retry_result = analyze_file(retry_path, AnalyzeOptions{});
  expect(retry_result.outcome == "failed" &&
             retry_result.outcome_evidence.find("another retry is pending") !=
                 std::string::npos,
         "FAILED_RETRY should fail the attempt and note the pending retry");
  std::filesystem::remove(retry_path);

  std::ostringstream json;
  RenderOptions render;
  render.format = OutputFormat::json;
  render_analysis(json, spark_result, render);
  expect(json.str().find("\"file\": \"spark_syslog.html\"") != std::string::npos,
         "JSON output should contain the scanned file name");
  expect(json.str().find("\"outcome\": \"succeeded\"") != std::string::npos,
         "JSON output should contain the resolved outcome");
  expect(json.str().find("\"scheduler\"") == std::string::npos,
         "JSON output should include scheduler fields only for scheduler logs");

  if (failures == 0) std::cout << "All tests passed\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
