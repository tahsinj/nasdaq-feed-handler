#include "book/fast_book.hpp"

#include <stdexcept>

namespace book {

FastBook::FastBook(const FastBookConfig& config)
    : num_locates_(config.num_locates),
      max_orders_(std::max<std::uint32_t>(config.max_orders, 1)),
      levels_(std::size_t{config.num_locates} * 2 * kWindow),
      bits_(std::size_t{config.num_locates} * 2 * kWords),
      pool_(max_orders_),
      orders_(std::size_t{max_orders_} * 2),
      far_arena_(std::max<std::size_t>(config.far_arena_bytes, std::size_t{1} << 20)),
      far_upstream_(far_arena_.data(), far_arena_.size(), std::pmr::null_memory_resource()),
      far_pool_(&far_upstream_),
      scratch_(kWindow) {
  const std::size_t n = std::size_t{num_locates_} * 2;
  ladders_.reserve(n);
  for (std::size_t i = 0; i < n; ++i) ladders_.emplace_back(&far_pool_);
}

std::size_t FastBook::preallocated_bytes() const noexcept {
  return levels_.size() * sizeof(Level) + bits_.size() * sizeof(std::uint64_t) +
         pool_.size() * sizeof(Order) + orders_.bytes() + far_arena_.size();
}

void FastBook::pool_exhausted() {
  throw std::length_error("order pool is full; rerun with a larger --max-orders");
}

// An on-grid order that would become the new best, or the first order on an empty side, moves
// the window instead of going to the far map, so the busy part of the book stays in the array.
template <Side S>
void FastBook::add_outside(Ladder& l, Price price, std::uint32_t tick, std::uint32_t shares) {
  if (tick != kOffGrid) {
    const bool empty = l.best < 0 && l.far.empty();
    if (empty || better<S>(price, best_quote<S>(l).price)) {
      recenter<S>(l, tick);
      add_in_window<S>(l, tick - l.base, shares);
      return;
    }
  }
  ++stats_.far_updates;
  Level& level = l.far[price];
  level.qty += shares;
  ++level.orders;
}

void FastBook::remove_far(Ladder& l, Price price, std::uint32_t shares, bool gone) {
  ++stats_.far_updates;
  const auto it = l.far.find(price);
  it->second.qty -= shares;
  if (gone && --it->second.orders == 0) l.far.erase(it);
}

template <Side S>
void FastBook::recenter(Ladder& l, std::uint32_t center_tick) {
  ++stats_.recenters;
  Level* levels = window(l);
  std::uint64_t* words = bits(l);
  std::size_t moved = 0;
  for (std::uint64_t nonzero = l.summary; nonzero != 0; nonzero &= nonzero - 1) {
    const auto w = static_cast<std::uint32_t>(std::countr_zero(nonzero));
    for (std::uint64_t m = words[w]; m != 0; m &= m - 1) {
      const std::uint32_t slot = w * 64 + static_cast<std::uint32_t>(std::countr_zero(m));
      scratch_[moved++] = {l.base + slot, levels[slot]};
      levels[slot] = {};
    }
    words[w] = 0;
  }
  l.summary = 0;
  l.base = center_tick > kWindow / 2 ? center_tick - kWindow / 2 : 0;

  for (std::size_t i = 0; i < moved; ++i) {
    const auto& [tick, level] = scratch_[i];
    const std::uint32_t slot = tick - l.base;
    if (slot < kWindow) {
      place(l, slot, level);
    } else {
      l.far.emplace(static_cast<Price>(price_of(tick)), level);
    }
  }

  const auto lo = static_cast<Price>(price_of(l.base));
  const std::uint64_t hi = price_of(std::uint64_t{l.base} + kWindow - 1);
  for (auto it = l.far.lower_bound(lo); it != l.far.end() && it->first <= hi;) {
    const std::uint32_t tick = tick_of(it->first);
    if (tick == kOffGrid) {
      ++it;
      continue;
    }
    place(l, tick - l.base, it->second);
    it = l.far.erase(it);
  }

  if constexpr (S == Side::Buy) {
    l.best = highest_at_or_below(l, kWindow - 1);
  } else {
    l.best = lowest_at_or_above(l, 0);
  }
}

template void FastBook::add_outside<Side::Buy>(Ladder&, Price, std::uint32_t, std::uint32_t);
template void FastBook::add_outside<Side::Sell>(Ladder&, Price, std::uint32_t, std::uint32_t);

}  // namespace book
