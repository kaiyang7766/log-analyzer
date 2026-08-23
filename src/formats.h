#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

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
};

using RecordConsumer = std::function<void(LogRecord&&)>;

DecodedInput decode_input(std::string_view input, const AnalyzeOptions& options,
                          const RecordConsumer& consume);
std::string_view input_format_name(InputFormat format);

}  // namespace logscope::detail
