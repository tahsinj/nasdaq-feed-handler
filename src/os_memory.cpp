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

#if defined(_WIN32)

// The SeLockMemoryPrivilege is needed before MEM_LARGE_PAGES can succeed. It comes from the
// "Lock pages in memory" user right, which an administrator grants and which takes effect at
// the next sign-in. Enabling it is harmless and idempotent; failure just leaves large pages off.
bool enable_lock_memory_privilege() {
  static const bool enabled = [] {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
      return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValue(nullptr, SE_LOCK_MEMORY_NAME, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) &&
              GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
  }();
  return enabled;
}

#endif

}  // namespace

OsBuffer::OsBuffer(std::size_t bytes) {
  if (bytes == 0) return;
#if defined(_WIN32)
  const std::size_t large = GetLargePageMinimum();
  if (large != 0 && enable_lock_memory_privilege()) {
    const std::size_t rounded = round_up(bytes, large);
    base_ =
        VirtualAlloc(nullptr, rounded, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
    if (base_ != nullptr) {
      reserved_ = rounded;
      large_pages_ = true;
    }
  }
  if (base_ == nullptr) {
    base_ = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (base_ == nullptr) throw std::bad_alloc();
    reserved_ = bytes;
  }
  data_ = base_;
#else
  const std::size_t reserved = round_up(bytes, kHugePage) + kHugePage;
  void* p = ::mmap(nullptr, reserved, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) throw std::bad_alloc();
  base_ = p;
  reserved_ = reserved;
  data_ = reinterpret_cast<void*>(round_up(reinterpret_cast<std::uintptr_t>(p), kHugePage));
#if defined(MADV_HUGEPAGE)
  large_pages_ = ::madvise(data_, round_up(bytes, kHugePage), MADV_HUGEPAGE) == 0;
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
      size_(std::exchange(other.size_, 0)),
      large_pages_(std::exchange(other.large_pages_, false)) {}

OsBuffer& OsBuffer::operator=(OsBuffer&& other) noexcept {
  if (this != &other) {
    release();
    base_ = std::exchange(other.base_, nullptr);
    reserved_ = std::exchange(other.reserved_, 0);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
    large_pages_ = std::exchange(other.large_pages_, false);
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
  large_pages_ = false;
}

}  // namespace util
