#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "util/os_memory.hpp"

namespace book {

// Open addressing map from order reference number to a pool slot. Linear probing with
// Fibonacci hashing; erase shifts later entries back instead of leaving tombstones, so probe
// lengths stay short through a full day of adds and deletes. Sized up front and never rehashed.
class OrderMap {
 public:
  static constexpr std::size_t kNotFound = ~std::size_t{0};

  explicit OrderMap(std::size_t min_capacity)
      : slots_(std::bit_ceil(std::max<std::size_t>(min_capacity, 16))),
        mask_(slots_.size() - 1),
        shift_(64 - std::countr_zero(slots_.size())) {
    std::memset(static_cast<void*>(slots_.data()), 0xff, slots_.size() * sizeof(Slot));
  }

  std::size_t find(std::uint64_t key) const noexcept {
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      const std::uint64_t k = slots_[i].key;
      if (k == kEmpty) return kNotFound;
      if (k == key) return i;
    }
  }

  // Returns false if the key is already present. The caller keeps the load factor under one half.
  bool insert(std::uint64_t key, std::uint32_t value) noexcept {
    if (key == kEmpty) [[unlikely]]
      return false;
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      Slot& s = slots_[i];
      if (s.key == kEmpty) {
        s = {key, value};
        ++size_;
        return true;
      }
      if (s.key == key) return false;
    }
  }

  std::uint32_t value(std::size_t entry) const noexcept { return slots_[entry].value; }

  void erase(std::size_t entry) noexcept {
    std::size_t hole = entry;
    for (std::size_t i = (entry + 1) & mask_;; i = (i + 1) & mask_) {
      const std::uint64_t k = slots_[i].key;
      if (k == kEmpty) break;
      // Entry i may fill the hole only if the hole lies between its home slot and i.
      if (((i - home(k)) & mask_) >= ((i - hole) & mask_)) {
        slots_[hole] = slots_[i];
        hole = i;
      }
    }
    slots_[hole].key = kEmpty;
    --size_;
  }

  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return slots_.size(); }
  std::size_t bytes() const noexcept { return slots_.size() * sizeof(Slot); }
  bool large_pages() const noexcept { return slots_.large_pages(); }

 private:
  struct Slot {
    std::uint64_t key;
    std::uint32_t value;
  };
  static constexpr std::uint64_t kEmpty = ~std::uint64_t{0};

  std::size_t home(std::uint64_t key) const noexcept {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
  }

  util::OsArray<Slot> slots_;
  std::size_t mask_;
  int shift_;
  std::size_t size_ = 0;
};

}  // namespace book
