#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory_resource>
#include <utility>
#include <vector>

#include "book/order_map.hpp"
#include "book/types.hpp"
#include "util/os_memory.hpp"

namespace book {

struct FastBookConfig {
  std::uint32_t num_locates = 0;  // highest stock locate in the feed plus one
  std::uint32_t max_orders = 1u << 23;
  std::size_t far_arena_bytes = std::size_t{256} << 20;
};

struct FastBookStats {
  std::uint64_t recenters = 0;
  std::uint64_t far_updates = 0;
};

// Order book built for replay speed.
//
// Each side of each symbol keeps a window of 4096 price ticks in a flat array, with a two level
// bitmap that finds the next best level in a few bit operations when the top one empties.
// Levels outside the window, or off the tick grid, live in a small ordered map, and the window
// recenters when the market moves past its edge. Orders sit in a fixed pool reached through an
// open addressing map. Every buffer is allocated in the constructor; after that no operation
// touches the heap.
class FastBook {
 public:
  static constexpr std::uint32_t kWindow = 4096;

  explicit FastBook(const FastBookConfig& config);
  FastBook(const FastBook&) = delete;
  FastBook& operator=(const FastBook&) = delete;

  void add(std::uint16_t locate, std::uint64_t ref, Side side, std::uint32_t shares, Price price);
  void execute(std::uint64_t ref, std::uint32_t shares) { reduce_order(ref, shares, false); }
  void cancel(std::uint64_t ref, std::uint32_t shares) { reduce_order(ref, shares, false); }
  void remove(std::uint64_t ref) { reduce_order(ref, 0, true); }
  void replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares, Price price);

  TopOfBook top(std::uint16_t locate) const noexcept;
  std::size_t live_orders() const noexcept { return orders_.size(); }
  std::size_t peak_orders() const noexcept { return peak_orders_; }
  const Anomalies& anomalies() const noexcept { return anomalies_; }
  const FastBookStats& stats() const noexcept { return stats_; }
  std::size_t preallocated_bytes() const noexcept;

 private:
  static constexpr std::uint32_t kWords = kWindow / 64;
  static constexpr std::uint32_t kOffGrid = ~std::uint32_t{0};
  static constexpr std::uint32_t kNoSlot = ~std::uint32_t{0};

  struct Level {
    Qty qty;
    std::uint32_t orders;
  };
  struct Order {
    Price price;  // next free slot while the pool slot is unused
    std::uint32_t shares;
    std::uint16_t locate;
    Side side;
  };
  struct alignas(64) Ladder {
    explicit Ladder(std::pmr::memory_resource* r) : far(r) {}
    std::uint64_t summary = 0;  // bit w is set when bitmap word w is non-zero
    std::uint32_t base = 0;     // tick held by window slot 0
    std::int32_t best = -1;     // window slot of the best level, -1 when the window is empty
    std::pmr::map<Price, Level> far;
  };

  // Sub-dollar prices tick in 1/10000ths and the rest in cents (Reg NMS Rule 612), so ticks
  // stay dense and monotonic across $1.
  static std::uint32_t tick_of(Price p) noexcept {
    if (p < 10000) return p;
    if (p % 100 != 0) return kOffGrid;
    return 10000 + (p - 10000) / 100;
  }
  static std::uint64_t price_of(std::uint64_t tick) noexcept {
    return tick < 10000 ? tick : 10000 + (tick - 10000) * 100;
  }

  template <Side S>
  static bool better(Price a, Price b) noexcept {
    if constexpr (S == Side::Buy) {
      return a > b;
    } else {
      return a < b;
    }
  }

  Ladder& ladder(std::uint16_t locate, Side side) noexcept {
    return ladders_[2 * std::size_t{locate} + static_cast<std::size_t>(side)];
  }
  std::size_t index_of(const Ladder& l) const noexcept {
    return static_cast<std::size_t>(&l - ladders_.data());
  }
  Level* window(const Ladder& l) const noexcept { return levels_.data() + index_of(l) * kWindow; }
  std::uint64_t* bits(const Ladder& l) const noexcept {
    return bits_.data() + index_of(l) * kWords;
  }

  std::uint32_t acquire();
  void release(std::uint32_t slot) noexcept;
  [[noreturn]] static void pool_exhausted();

  void reduce_order(std::uint64_t ref, std::uint32_t shares, bool whole);
  void add_shares(Ladder& l, Side side, Price price, std::uint32_t shares);
  void remove_shares(Ladder& l, Side side, Price price, std::uint32_t shares, bool gone);
  template <Side S>
  void add_shares(Ladder& l, Price price, std::uint32_t shares);
  template <Side S>
  void remove_shares(Ladder& l, Price price, std::uint32_t shares, bool gone);
  template <Side S>
  void add_in_window(Ladder& l, std::uint32_t slot, std::uint32_t shares) noexcept;
  template <Side S>
  Quote best_quote(const Ladder& l) const noexcept;

  template <Side S>
  void add_outside(Ladder& l, Price price, std::uint32_t tick, std::uint32_t shares);
  void remove_far(Ladder& l, Price price, std::uint32_t shares, bool gone);
  template <Side S>
  void recenter(Ladder& l, std::uint32_t center_tick);

  void place(Ladder& l, std::uint32_t slot, const Level& level) noexcept;
  void set_bit(Ladder& l, std::uint32_t slot) noexcept;
  void clear_bit(Ladder& l, std::uint32_t slot) noexcept;
  std::int32_t highest_at_or_below(const Ladder& l, std::uint32_t slot) const noexcept;
  std::int32_t lowest_at_or_above(const Ladder& l, std::uint32_t slot) const noexcept;

  std::uint32_t num_locates_;
  std::uint32_t max_orders_;
  util::OsArray<Level> levels_;
  util::OsArray<std::uint64_t> bits_;
  util::OsArray<Order> pool_;
  OrderMap orders_;
  util::OsBuffer far_arena_;
  std::pmr::monotonic_buffer_resource far_upstream_;
  std::pmr::unsynchronized_pool_resource far_pool_;
  std::vector<Ladder> ladders_;
  std::vector<std::pair<std::uint32_t, Level>> scratch_;
  std::uint32_t free_head_ = kNoSlot;
  std::uint32_t next_unused_ = 0;
  std::size_t peak_orders_ = 0;
  Anomalies anomalies_;
  FastBookStats stats_;
};

inline std::uint32_t FastBook::acquire() {
  if (free_head_ != kNoSlot) {
    const std::uint32_t slot = free_head_;
    free_head_ = pool_[slot].price;
    return slot;
  }
  if (next_unused_ == max_orders_) [[unlikely]]
    pool_exhausted();
  return next_unused_++;
}

inline void FastBook::release(std::uint32_t slot) noexcept {
  pool_[slot].price = free_head_;
  free_head_ = slot;
}

inline void FastBook::add(std::uint16_t locate, std::uint64_t ref, Side side, std::uint32_t shares,
                          Price price) {
  if (shares == 0) [[unlikely]] {
    ++anomalies_.zero_shares;
    return;
  }
  if (locate >= num_locates_) [[unlikely]] {
    ++anomalies_.bad_locate;
    return;
  }
  const std::uint32_t slot = acquire();
  if (!orders_.insert(ref, slot)) [[unlikely]] {
    release(slot);
    ++anomalies_.duplicate_order;
    return;
  }
  peak_orders_ = std::max(peak_orders_, orders_.size());
  pool_[slot] = Order{price, shares, locate, side};
  add_shares(ladder(locate, side), side, price, shares);
}

inline void FastBook::reduce_order(std::uint64_t ref, std::uint32_t shares, bool whole) {
  const std::size_t entry = orders_.find(ref);
  if (entry == OrderMap::kNotFound) [[unlikely]] {
    ++anomalies_.unknown_order;
    return;
  }
  const std::uint32_t slot = orders_.value(entry);
  Order& o = pool_[slot];
  if (whole) {
    shares = o.shares;
  } else if (shares > o.shares) [[unlikely]] {
    ++anomalies_.overfill;
    shares = o.shares;
  }
  o.shares -= shares;
  const bool gone = o.shares == 0;
  remove_shares(ladder(o.locate, o.side), o.side, o.price, shares, gone);
  if (gone) {
    orders_.erase(entry);
    release(slot);
  }
}

inline void FastBook::replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares,
                              Price price) {
  const std::size_t entry = orders_.find(old_ref);
  if (entry == OrderMap::kNotFound) [[unlikely]] {
    ++anomalies_.unknown_order;
    return;
  }
  const std::uint32_t slot = orders_.value(entry);
  Order& o = pool_[slot];
  Ladder& l = ladder(o.locate, o.side);
  remove_shares(l, o.side, o.price, o.shares, true);
  orders_.erase(entry);
  if (shares == 0) [[unlikely]] {
    ++anomalies_.zero_shares;
    release(slot);
    return;
  }
  if (!orders_.insert(new_ref, slot)) [[unlikely]] {
    ++anomalies_.duplicate_order;
    release(slot);
    return;
  }
  o.price = price;
  o.shares = shares;
  add_shares(l, o.side, price, shares);
}

inline TopOfBook FastBook::top(std::uint16_t locate) const noexcept {
  if (locate >= num_locates_) return {};
  const Ladder* l = &ladders_[2 * std::size_t{locate}];
  return {best_quote<Side::Buy>(l[0]), best_quote<Side::Sell>(l[1])};
}

inline void FastBook::add_shares(Ladder& l, Side side, Price price, std::uint32_t shares) {
  if (side == Side::Buy) {
    add_shares<Side::Buy>(l, price, shares);
  } else {
    add_shares<Side::Sell>(l, price, shares);
  }
}

inline void FastBook::remove_shares(Ladder& l, Side side, Price price, std::uint32_t shares,
                                    bool gone) {
  if (side == Side::Buy) {
    remove_shares<Side::Buy>(l, price, shares, gone);
  } else {
    remove_shares<Side::Sell>(l, price, shares, gone);
  }
}

template <Side S>
inline void FastBook::add_shares(Ladder& l, Price price, std::uint32_t shares) {
  const std::uint32_t tick = tick_of(price);
  const std::uint32_t slot = tick - l.base;
  if (slot < kWindow) [[likely]] {
    add_in_window<S>(l, slot, shares);
  } else {
    add_outside<S>(l, price, tick, shares);
  }
}

template <Side S>
inline void FastBook::add_in_window(Ladder& l, std::uint32_t slot, std::uint32_t shares) noexcept {
  Level& level = window(l)[slot];
  level.qty += shares;
  if (level.orders++ != 0) return;
  set_bit(l, slot);
  const auto s = static_cast<std::int32_t>(slot);
  if constexpr (S == Side::Buy) {
    if (s > l.best) l.best = s;
  } else {
    if (l.best < 0 || s < l.best) l.best = s;
  }
}

template <Side S>
inline void FastBook::remove_shares(Ladder& l, Price price, std::uint32_t shares, bool gone) {
  const std::uint32_t slot = tick_of(price) - l.base;
  if (slot >= kWindow) [[unlikely]] {
    remove_far(l, price, shares, gone);
    return;
  }
  Level& level = window(l)[slot];
  level.qty -= shares;
  if (!gone || --level.orders != 0) return;
  clear_bit(l, slot);
  if (static_cast<std::int32_t>(slot) != l.best) return;
  if constexpr (S == Side::Buy) {
    l.best = highest_at_or_below(l, slot);
  } else {
    l.best = lowest_at_or_above(l, slot);
  }
}

template <Side S>
inline Quote FastBook::best_quote(const Ladder& l) const noexcept {
  Quote q;
  if (l.best >= 0) {
    const Level& level = window(l)[l.best];
    const std::uint64_t tick = std::uint64_t{l.base} + static_cast<std::uint32_t>(l.best);
    q = {static_cast<Price>(price_of(tick)), level.orders, level.qty};
  }
  if (!l.far.empty()) {
    const auto& [price, level] = S == Side::Buy ? *std::prev(l.far.end()) : *l.far.begin();
    if (q.qty == 0 || better<S>(price, q.price)) q = {price, level.orders, level.qty};
  }
  return q;
}

inline void FastBook::place(Ladder& l, std::uint32_t slot, const Level& level) noexcept {
  window(l)[slot] = level;
  set_bit(l, slot);
}

inline void FastBook::set_bit(Ladder& l, std::uint32_t slot) noexcept {
  const std::uint32_t w = slot >> 6;
  bits(l)[w] |= std::uint64_t{1} << (slot & 63);
  l.summary |= std::uint64_t{1} << w;
}

inline void FastBook::clear_bit(Ladder& l, std::uint32_t slot) noexcept {
  std::uint64_t* words = bits(l);
  const std::uint32_t w = slot >> 6;
  words[w] &= ~(std::uint64_t{1} << (slot & 63));
  if (words[w] == 0) l.summary &= ~(std::uint64_t{1} << w);
}

inline std::int32_t FastBook::highest_at_or_below(const Ladder& l,
                                                  std::uint32_t slot) const noexcept {
  const std::uint64_t* words = bits(l);
  std::uint32_t w = slot >> 6;
  std::uint64_t m = words[w] & ((std::uint64_t{2} << (slot & 63)) - 1);
  if (m == 0) {
    const std::uint64_t below = l.summary & ((std::uint64_t{1} << w) - 1);
    if (below == 0) return -1;
    w = 63 - static_cast<std::uint32_t>(std::countl_zero(below));
    m = words[w];
  }
  return static_cast<std::int32_t>(w * 64 + 63 - static_cast<std::uint32_t>(std::countl_zero(m)));
}

inline std::int32_t FastBook::lowest_at_or_above(const Ladder& l,
                                                 std::uint32_t slot) const noexcept {
  const std::uint64_t* words = bits(l);
  std::uint32_t w = slot >> 6;
  std::uint64_t m = words[w] & (~std::uint64_t{0} << (slot & 63));
  if (m == 0) {
    const std::uint64_t above = l.summary & ~((std::uint64_t{2} << w) - 1);
    if (above == 0) return -1;
    w = static_cast<std::uint32_t>(std::countr_zero(above));
    m = words[w];
  }
  return static_cast<std::int32_t>(w * 64 + static_cast<std::uint32_t>(std::countr_zero(m)));
}

}  // namespace book
