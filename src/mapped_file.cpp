#include "itch/mapped_file.hpp"

#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace itch {
namespace {

volatile unsigned g_prefault_sink = 0;

[[noreturn]] void fail(const std::filesystem::path& path, const char* call) {
#if defined(_WIN32)
  const std::string reason = "error " + std::to_string(GetLastError());
#else
  const std::string reason = std::strerror(errno);
#endif
  throw std::runtime_error(std::string(call) + " failed for " + path.string() + ": " + reason);
}

}  // namespace

MappedFile::MappedFile(const std::filesystem::path& path) {
  try {
    open(path);
  } catch (...) {
    close();
    throw;
  }
}

MappedFile::~MappedFile() { close(); }

#if defined(_WIN32)

void MappedFile::open(const std::filesystem::path& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) fail(path, "CreateFileW");
  file_ = file;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size)) fail(path, "GetFileSizeEx");
  size_ = static_cast<std::size_t>(size.QuadPart);
  if (size_ == 0) return;
  mapping_ = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (mapping_ == nullptr) fail(path, "CreateFileMappingW");
  data_ = static_cast<const char*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
  if (data_ == nullptr) fail(path, "MapViewOfFile");
}

void MappedFile::close() noexcept {
  if (data_ != nullptr) UnmapViewOfFile(data_);
  if (mapping_ != nullptr) CloseHandle(mapping_);
  if (file_ != nullptr) CloseHandle(file_);
  data_ = nullptr;
  mapping_ = nullptr;
  file_ = nullptr;
  size_ = 0;
}

#else

void MappedFile::open(const std::filesystem::path& path) {
  fd_ = ::open(path.c_str(), O_RDONLY);
  if (fd_ < 0) fail(path, "open");
  struct stat st{};
  if (::fstat(fd_, &st) != 0) fail(path, "fstat");
  size_ = static_cast<std::size_t>(st.st_size);
  if (size_ == 0) return;
  void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
  if (p == MAP_FAILED) fail(path, "mmap");
  data_ = static_cast<const char*>(p);
}

void MappedFile::close() noexcept {
  if (data_ != nullptr) ::munmap(const_cast<char*>(data_), size_);
  if (fd_ >= 0) ::close(fd_);
  data_ = nullptr;
  fd_ = -1;
  size_ = 0;
}

#endif

void MappedFile::prefault() const {
  if (size_ == 0) return;
#if defined(MADV_WILLNEED)
  ::madvise(const_cast<char*>(data_), size_, MADV_WILLNEED);
#endif
  unsigned sum = 0;
  for (std::size_t i = 0; i < size_; i += 4096) sum += static_cast<unsigned char>(data_[i]);
  g_prefault_sink = sum;
}

}  // namespace itch
