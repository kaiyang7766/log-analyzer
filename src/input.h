#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "logscope/model.h"

namespace logscope::detail {

class InputBuffer {
 public:
  InputBuffer(const std::filesystem::path& path, IoMode mode);
  ~InputBuffer();

  InputBuffer(const InputBuffer&) = delete;
  InputBuffer& operator=(const InputBuffer&) = delete;

  const char* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }

 private:
  std::string owned_;
  const char* data_ = nullptr;
  std::size_t size_ = 0;
  void* mapping_ = nullptr;
  int fd_ = -1;
};

}  // namespace logscope::detail
