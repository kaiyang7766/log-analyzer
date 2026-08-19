#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "logscope/analyzer.h"

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
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
  expect(result.malformed == 1, "malformed lines should be counted");
  expect(result.matched == 2, "valid matching records should be counted");
  expect(result.errors.size() == 1, "error should be grouped");
  expect(result.endpoint_latencies.at("GET /a").size() == 2, "latencies should be retained");

  AnalyzeOptions filtered;
  filtered.filters.service = "db";
  const auto filtered_result = analyze_file(path, filtered);
  expect(filtered_result.matched == 1, "service filter should select one record");
  std::filesystem::remove(path);

  if (failures == 0) std::cout << "All tests passed\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
