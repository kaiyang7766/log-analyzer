#pragma once

#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "logscope/model.h"

namespace logscope {

struct RenderOptions {
  OutputFormat format = OutputFormat::text;
  std::string group_by;
  std::size_t limit = 20;
};

void render_analysis(std::ostream& out, const AnalysisResult& result, const RenderOptions& options);
void render_errors(std::ostream& out, const AnalysisResult& result, const RenderOptions& options);
void render_latency(std::ostream& out, const AnalysisResult& result, const RenderOptions& options);
void render_records(std::ostream& out, const std::vector<LogRecord>& records,
                    OutputFormat format, std::string_view title);
void render_contexts(std::ostream& out, const std::vector<std::vector<LogRecord>>& contexts,
                     OutputFormat format, std::string_view needle);

}  // namespace logscope
