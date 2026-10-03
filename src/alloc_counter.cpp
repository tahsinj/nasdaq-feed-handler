#include "util/alloc_counter.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

std::atomic<std::uint64_t> g_allocations{0};

void* allocate(std::size_t n) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
  throw std::bad_alloc();
}

void* allocate_aligned(std::size_t n, std::align_val_t align) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  const auto a = std::max(static_cast<std::size_t>(align), sizeof(void*));
#if defined(_WIN32)
  if (void* p = _aligned_malloc(n == 0 ? 1 : n, a)) return p;
#else
  void* p = nullptr;
  if (posix_memalign(&p, a, n == 0 ? 1 : n) == 0) return p;
#endif
  throw std::bad_alloc();
}

void free_aligned(void* p) noexcept {
#if defined(_WIN32)
  _aligned_free(p);
#else
  std::free(p);
#endif
}

}  // namespace

namespace util {

std::uint64_t heap_allocations() noexcept { return g_allocations.load(std::memory_order_relaxed); }

}  // namespace util

void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void* operator new(std::size_t n, std::align_val_t a) { return allocate_aligned(n, a); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocate_aligned(n, a); }

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return allocate(n);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return allocate(n);
  } catch (...) {
    return nullptr;
  }
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  try {
    return allocate_aligned(n, a);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  try {
    return allocate_aligned(n, a);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { free_aligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { free_aligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { free_aligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { free_aligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { free_aligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  free_aligned(p);
}
