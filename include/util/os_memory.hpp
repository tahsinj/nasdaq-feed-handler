#pragma once

#include <cstddef>
#include <type_traits>

namespace util {

// Zeroed memory mapped straight from the OS and touched up front, so the hot path never takes
// a page fault. On Linux the region is 2 MB aligned and marked for transparent huge pages,
// which keeps large random-access tables from thrashing the TLB.
class OsBuffer {
 public:
  OsBuffer() = default;
  explicit OsBuffer(std::size_t bytes);
  ~OsBuffer();
  OsBuffer(OsBuffer&& other) noexcept;
  OsBuffer& operator=(OsBuffer&& other) noexcept;
  OsBuffer(const OsBuffer&) = delete;
  OsBuffer& operator=(const OsBuffer&) = delete;

  void* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }

  // Whether the buffer is backed by large pages: on Linux whether madvise(MADV_HUGEPAGE)
  // was accepted, on Windows whether the MEM_LARGE_PAGES allocation succeeded.
  bool large_pages() const noexcept { return large_pages_; }

 private:
  void release() noexcept;

  void* base_ = nullptr;
  std::size_t reserved_ = 0;
  void* data_ = nullptr;
  std::size_t size_ = 0;
  bool large_pages_ = false;
};

template <class T>
class OsArray {
  static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);

 public:
  OsArray() = default;
  explicit OsArray(std::size_t n) : buffer_(n * sizeof(T)), size_(n) {}

  T* data() const noexcept { return static_cast<T*>(buffer_.data()); }
  T& operator[](std::size_t i) const noexcept { return data()[i]; }
  std::size_t size() const noexcept { return size_; }
  bool large_pages() const noexcept { return buffer_.large_pages(); }

 private:
  OsBuffer buffer_;
  std::size_t size_ = 0;
};

}  // namespace util
