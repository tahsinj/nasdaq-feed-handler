#pragma once

#include <cstddef>
#include <filesystem>

namespace itch {

// Read-only memory mapping of a whole file (mmap on POSIX, MapViewOfFile on Windows).
class MappedFile {
 public:
  explicit MappedFile(const std::filesystem::path& path);
  ~MappedFile();
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  const char* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }

  // Touches every page so a timed replay does not wait on the disk or take page faults.
  void prefault() const;

 private:
  void open(const std::filesystem::path& path);
  void close() noexcept;

  const char* data_ = nullptr;
  std::size_t size_ = 0;
#if defined(_WIN32)
  void* file_ = nullptr;
  void* mapping_ = nullptr;
#else
  int fd_ = -1;
#endif
};

}  // namespace itch
