#pragma once

#include <cstdint>

namespace book {

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

using Price = std::uint32_t;  // ITCH Price(4): dollars times 10,000
using Qty = std::uint64_t;

// qty == 0 means the side is empty.
struct Quote {
  Price price = 0;
  std::uint32_t orders = 0;
  Qty qty = 0;
  bool operator==(const Quote&) const = default;
};

struct TopOfBook {
  Quote bid;
  Quote ask;
  bool operator==(const TopOfBook&) const = default;
};

// Feed events a book could not apply as written. Exchange data should produce none, so any
// count here points at a parsing bug or a gap in the feed.
struct Anomalies {
  std::uint64_t unknown_order = 0;
  std::uint64_t duplicate_order = 0;
  std::uint64_t zero_shares = 0;
  std::uint64_t overfill = 0;
  std::uint64_t bad_locate = 0;

  std::uint64_t total() const noexcept {
    return unknown_order + duplicate_order + zero_shares + overfill + bad_locate;
  }
  bool operator==(const Anomalies&) const = default;
};

inline Side side_from_itch(char c) noexcept { return c == 'S' ? Side::Sell : Side::Buy; }

}  // namespace book
