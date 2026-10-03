#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace util {

// Lock-free single producer, single consumer ring.
//
// Each side owns one index and keeps a private copy of the other side's index, reloading it
// only when the ring looks full or empty. In steady state the shared lines cross between cores
// once per batch rather than once per item.
template <class T>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  explicit SpscRing(std::size_t capacity)
      : mask_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity) - 1),
        slots_(std::make_unique<T[]>(mask_ + 1)) {}

  bool try_push(const T& value) noexcept {
    const std::size_t head = producer_.head.load(std::memory_order_relaxed);
    if (head - producer_.tail_cache > mask_) {
      producer_.tail_cache = consumer_.tail.load(std::memory_order_acquire);
      if (head - producer_.tail_cache > mask_) return false;
    }
    slots_[head & mask_] = value;
    producer_.head.store(head + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) noexcept {
    const std::size_t tail = consumer_.tail.load(std::memory_order_relaxed);
    if (tail == consumer_.head_cache) {
      consumer_.head_cache = producer_.head.load(std::memory_order_acquire);
      if (tail == consumer_.head_cache) return false;
    }
    out = slots_[tail & mask_];
    consumer_.tail.store(tail + 1, std::memory_order_release);
    return true;
  }

  std::size_t capacity() const noexcept { return mask_ + 1; }

 private:
  // 128 bytes rather than 64: Intel's adjacent-line prefetcher fetches lines in pairs.
  struct alignas(128) Producer {
    std::atomic<std::size_t> head{0};
    std::size_t tail_cache = 0;
  };
  struct alignas(128) Consumer {
    std::atomic<std::size_t> tail{0};
    std::size_t head_cache = 0;
  };

  Producer producer_;
  Consumer consumer_;
  const std::size_t mask_;
  std::unique_ptr<T[]> slots_;
};

}  // namespace util
