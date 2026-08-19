#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "logscope/model.h"

namespace logscope {

AnalysisResult analyze_file(const std::filesystem::path& path, const AnalyzeOptions& options);
std::string normalize_error(std::string_view message, std::string_view stack_trace = {});
Percentiles calculate_percentiles(std::vector<double> values);
std::vector<LogRecord> select_trace(const AnalysisResult& result, std::string_view trace_id);
std::vector<std::vector<LogRecord>> select_contexts(const AnalysisResult& result,
                                                    std::string_view needle,
                                                    std::size_t before,
                                                    std::size_t after);

}  // namespace logscope
