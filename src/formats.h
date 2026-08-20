#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "logscope/model.h"

namespace logscope::detail {

struct DecodedInput {
  InputFormat format = InputFormat::auto_detect;
  std::string format_name;
  std::string container_name = "plain-text";
  std::uint64_t physical_lines = 0;
  std::uint64_t logical_events = 0;
  std::uint64_t continuation_lines = 0;
  std::uint64_t malformed = 0;
  bool reverse_order = false;
  std::vector<LogRecord> records;
};

DecodedInput decode_input(std::string_view input, const AnalyzeOptions& options);
std::string_view input_format_name(InputFormat format);

}  // namespace logscope::detail
