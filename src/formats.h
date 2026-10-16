#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "log_analyzer/model.h"

namespace log_analyzer::detail {

// The log content inside a container (NUL padding, HTML <xmp>), with the
// offset of its first byte in the original input.
struct ContainerView {
  std::string_view content;
  std::size_t base_offset = 0;
  std::string name = "plain-text";
};

struct DecodedInput {
  InputFormat format = InputFormat::auto_detect;
  std::string format_name;
  std::string container_name = "plain-text";
  std::uint64_t physical_lines = 0;
  std::uint64_t logical_events = 0;
  std::uint64_t continuation_lines = 0;
  std::uint64_t malformed = 0;
  bool reverse_order = false;
  SchedulerSummary scheduler;
};

using RecordConsumer = std::function<void(LogRecord&&)>;

ContainerView unwrap_container(std::string_view input);
DecodedInput decode_input(const ContainerView& container,
                          const AnalyzeOptions& options,
                          const RecordConsumer& consume);
std::string_view input_format_name(InputFormat format);

}  // namespace log_analyzer::detail
