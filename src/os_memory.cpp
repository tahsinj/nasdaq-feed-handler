#include "util/os_memory.hpp"

#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace util {
namespace {

constexpr std::size_t kHugePage = std::size_t{2} << 20;

constexpr std::size_t round_up(std::size_t n, std::size_t align) {
  return (n + align - 1) / align * align;
}

}  // namespace

OsBuffer::OsBuffer(std::size_t bytes) {
  if (bytes == 0) return;
#if defined(_WIN32)
  base_ = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (base_ == nullptr) throw std::bad_alloc();
  reserved_ = bytes;
  data_ = base_;
#else
  const std::size_t reserved = round_up(bytes, kHugePage) + kHugePage;
  void* p = ::mmap(nullptr, reserved, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) throw std::bad_alloc();
  base_ = p;
  reserved_ = reserved;
  data_ = reinterpret_cast<void*>(round_up(reinterpret_cast<std::uintptr_t>(p), kHugePage));
#if defined(MADV_HUGEPAGE)
  ::madvise(data_, round_up(bytes, kHugePage), MADV_HUGEPAGE);
#endif
#endif
  size_ = bytes;
  std::memset(data_, 0, bytes);
}

OsBuffer::~OsBuffer() { release(); }

OsBuffer::OsBuffer(OsBuffer&& other) noexcept
    : base_(std::exchange(other.base_, nullptr)),
      reserved_(std::exchange(other.reserved_, 0)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

OsBuffer& OsBuffer::operator=(OsBuffer&& other) noexcept {
  if (this != &other) {
    release();
    base_ = std::exchange(other.base_, nullptr);
    reserved_ = std::exchange(other.reserved_, 0);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

void OsBuffer::release() noexcept {
  if (base_ == nullptr) return;
#if defined(_WIN32)
  VirtualFree(base_, 0, MEM_RELEASE);
#else
  ::munmap(base_, reserved_);
#endif
  base_ = nullptr;
  reserved_ = 0;
  data_ = nullptr;
  size_ = 0;
}

}  // namespace util
