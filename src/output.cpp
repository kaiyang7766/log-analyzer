#include "logscope/output.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

#include "logscope/analyzer.h"

namespace logscope {
namespace {

template <typename Map>
auto sorted_counts(const Map& counts) {
  std::vector<std::pair<std::string, std::uint64_t>> rows(counts.begin(), counts.end());
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return left.second != right.second ? left.second > right.second : left.first < right.first;
  });
  return rows;
}

std::string compact(std::string value, std::size_t width = 96) {
  std::replace(value.begin(), value.end(), '\n', ' ');
  std::replace(value.begin(), value.end(), '\r', ' ');
  if (value.size() > width) value = value.substr(0, width - 3) + "...";
  return value;
}

void heading(std::ostream& out, OutputFormat format, std::string_view title) {
  if (format == OutputFormat::markdown) out << "## " << title << "\n\n";
  else out << title << "\n" << std::string(title.size(), '=') << "\n";
}

void render_count_table(std::ostream& out, const auto& rows, OutputFormat format,
                        std::string_view name, std::size_t limit) {
  if (format == OutputFormat::markdown) {
    out << "| " << name << " | Count |\n|---|---:|\n";
    for (std::size_t i = 0; i < std::min(limit, rows.size()); ++i)
      out << "| " << compact(rows[i].first) << " | " << rows[i].second << " |\n";
    out << '\n';
  } else {
    for (std::size_t i = 0; i < std::min(limit, rows.size()); ++i)
      out << std::setw(8) << rows[i].second << "  " << compact(rows[i].first) << '\n';
    if (rows.empty()) out << "(none)\n";
    out << '\n';
  }
}

using ErrorRow = std::pair<std::string, const ErrorStats*>;

std::vector<ErrorRow> sorted_errors(const AnalysisResult& result) {
  std::vector<ErrorRow> rows;
  rows.reserve(result.errors.size());
  for (const auto& [key, stats] : result.errors) rows.emplace_back(key, &stats);
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    const auto left_delta = static_cast<std::int64_t>(left.second->recent) -
                            static_cast<std::int64_t>(left.second->previous);
    const auto right_delta = static_cast<std::int64_t>(right.second->recent) -
                             static_cast<std::int64_t>(right.second->previous);
    return left_delta != right_delta ? left_delta > right_delta
                                     : left.second->total > right.second->total;
  });
  return rows;
}

void render_error_rows(std::ostream& out, const std::vector<ErrorRow>& rows,
                       OutputFormat format, std::size_t limit) {
  if (format == OutputFormat::markdown) {
    out << "| Normalized error | Previous half | Recent half | Change | Total |\n"
           "|---|---:|---:|---:|---:|\n";
  }
  for (std::size_t i = 0; i < std::min(limit, rows.size()); ++i) {
    const auto& [name, stats] = rows[i];
    const auto delta = static_cast<std::int64_t>(stats->recent) -
                       static_cast<std::int64_t>(stats->previous);
    if (format == OutputFormat::markdown) {
      out << "| " << compact(name) << " | " << stats->previous << " | " << stats->recent
          << " | " << std::showpos << delta << std::noshowpos << " | " << stats->total << " |\n";
    } else {
      out << std::setw(8) << stats->total << " total  " << std::setw(6) << std::showpos << delta
          << std::noshowpos << " recent change  " << compact(name) << '\n';
      if (!stats->example.empty()) out << "          example: " << compact(stats->example) << '\n';
    }
  }
  if (rows.empty()) out << (format == OutputFormat::markdown ? "| _No errors_ | 0 | 0 | 0 | 0 |\n" : "(none)\n");
  out << '\n';
}

void render_latency_map(std::ostream& out,
                        const std::unordered_map<std::string, std::vector<double>>& groups,
                        OutputFormat format, std::string_view group_name, std::size_t limit) {
  struct Row { std::string name; Percentiles values; };
  std::vector<Row> rows;
  rows.reserve(groups.size());
  for (const auto& [name, values] : groups) rows.push_back({name, calculate_percentiles(values)});
  std::sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
    return left.values.p99 != right.values.p99 ? left.values.p99 > right.values.p99
                                               : left.name < right.name;
  });

  if (format == OutputFormat::markdown) {
    out << "| " << group_name << " | Samples | p50 ms | p95 ms | p99 ms | Max ms |\n"
           "|---|---:|---:|---:|---:|---:|\n";
  }
  out << std::fixed << std::setprecision(1);
  for (std::size_t i = 0; i < std::min(limit, rows.size()); ++i) {
    const auto& row = rows[i];
    if (format == OutputFormat::markdown) {
      out << "| " << compact(row.name) << " | " << row.values.count << " | " << row.values.p50
          << " | " << row.values.p95 << " | " << row.values.p99 << " | " << row.values.max << " |\n";
    } else {
      out << std::setw(8) << row.values.count << "  p50=" << std::setw(8) << row.values.p50
          << "  p95=" << std::setw(8) << row.values.p95 << "  p99=" << std::setw(8)
          << row.values.p99 << "  max=" << std::setw(8) << row.values.max << "  " << row.name << '\n';
    }
  }
  if (rows.empty()) out << (format == OutputFormat::markdown ? "| _No latency samples_ | 0 | 0 | 0 | 0 | 0 |\n" : "(none)\n");
  out << "\n";
}

}  // namespace

void render_analysis(std::ostream& out, const AnalysisResult& result, const RenderOptions& options) {
  if (options.format == OutputFormat::json) return;
  heading(out, options.format, "Incident summary");
  if (options.format == OutputFormat::markdown) {
    out << "- Parsed lines: " << result.lines << '\n'
        << "- Matching events: " << result.matched << '\n'
        << "- Malformed lines skipped: " << result.malformed << "\n\n";
  } else {
    out << "Parsed " << result.lines << " lines; " << result.matched << " matched; "
        << result.malformed << " malformed.\n\n";
  }
  heading(out, options.format, "Errors increasing in the recent half");
  render_error_rows(out, sorted_errors(result), options.format, options.limit);
  heading(out, options.format, "Services involved");
  render_count_table(out, sorted_counts(result.services), options.format, "Service", options.limit);
  heading(out, options.format, "Endpoints involved");
  render_count_table(out, sorted_counts(result.endpoints), options.format, "Endpoint", options.limit);
  heading(out, options.format, "Endpoint latency");
  render_latency_map(out, result.endpoint_latencies, options.format, "Endpoint", options.limit);
}

void render_errors(std::ostream& out, const AnalysisResult& result, const RenderOptions& options) {
  if (options.format == OutputFormat::json) return;
  heading(out, options.format, "Error groups");
  if (options.group_by == "service") {
    std::unordered_map<std::string, std::uint64_t> counts;
    for (const auto& [_, stats] : result.errors) {
      for (const auto& [service, count] : stats.services) counts[service] += count;
    }
    render_count_table(out, sorted_counts(counts), options.format, "Service", options.limit);
  } else if (options.group_by == "endpoint") {
    std::unordered_map<std::string, std::uint64_t> counts;
    for (const auto& [_, stats] : result.errors) {
      for (const auto& [endpoint, count] : stats.endpoints) counts[endpoint] += count;
    }
    render_count_table(out, sorted_counts(counts), options.format, "Endpoint", options.limit);
  } else {
    render_error_rows(out, sorted_errors(result), options.format, options.limit);
  }
}

void render_latency(std::ostream& out, const AnalysisResult& result, const RenderOptions& options) {
  if (options.format == OutputFormat::json) return;
  const bool by_service = options.group_by == "service";
  heading(out, options.format, by_service ? "Service latency" : "Endpoint latency");
  render_latency_map(out, by_service ? result.service_latencies : result.endpoint_latencies,
                     options.format, by_service ? "Service" : "Endpoint", options.limit);
}

void render_records(std::ostream&, const std::vector<LogRecord>&, OutputFormat, std::string_view) {}
void render_contexts(std::ostream&, const std::vector<std::vector<LogRecord>>&, OutputFormat,
                     std::string_view) {}

}  // namespace logscope
