#include "input.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace logscope::detail {

InputBuffer::InputBuffer(const std::filesystem::path& path, IoMode mode) {
#if defined(__unix__) || defined(__APPLE__)
  if (mode == IoMode::mmap) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) throw std::runtime_error("cannot open input: " + path.string());
    struct stat info {};
    if (::fstat(fd_, &info) != 0) {
      const auto error = std::string(std::strerror(errno));
      ::close(fd_);
      fd_ = -1;
      throw std::runtime_error("cannot stat input: " + error);
    }
    size_ = static_cast<std::size_t>(info.st_size);
    if (size_ == 0) return;
    mapping_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapping_ == MAP_FAILED) {
      mapping_ = nullptr;
      const auto error = std::string(std::strerror(errno));
      ::close(fd_);
      fd_ = -1;
      throw std::runtime_error("cannot mmap input: " + error);
    }
    data_ = static_cast<const char*>(mapping_);
    return;
  }
#else
  (void)mode;
#endif

  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open input: " + path.string());
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  input.seekg(0, std::ios::beg);
  if (length > 0) {
    owned_.resize(static_cast<std::size_t>(length));
    input.read(owned_.data(), static_cast<std::streamsize>(owned_.size()));
    if (!input) throw std::runtime_error("failed while reading input: " + path.string());
  }
  data_ = owned_.data();
  size_ = owned_.size();
}

InputBuffer::~InputBuffer() {
#if defined(__unix__) || defined(__APPLE__)
  if (mapping_) ::munmap(mapping_, size_);
  if (fd_ >= 0) ::close(fd_);
#endif
}

}  // namespace logscope::detail
