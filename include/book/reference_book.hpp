#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

#include "book/types.hpp"

namespace book {

// Plain standard containers: an ordered map of price levels per side and a hash map of live
// orders. Slow, but simple enough to trust as the baseline for verification.
class ReferenceBook {
 public:
  ReferenceBook();

  void add(std::uint16_t locate, std::uint64_t ref, Side side, std::uint32_t shares, Price price);
  void execute(std::uint64_t ref, std::uint32_t shares);
  void cancel(std::uint64_t ref, std::uint32_t shares);
  void remove(std::uint64_t ref);
  void replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares, Price price);

  TopOfBook top(std::uint16_t locate) const;
  std::size_t live_orders() const noexcept { return orders_.size(); }
  std::size_t peak_orders() const noexcept { return peak_orders_; }
  const Anomalies& anomalies() const noexcept { return anomalies_; }

 private:
  struct Order {
    std::uint16_t locate;
    Side side;
    Price price;
    std::uint32_t shares;
  };
  struct Level {
    Qty qty = 0;
    std::uint32_t orders = 0;
  };
  struct Symbol {
    std::map<Price, Level, std::greater<>> bids;
    std::map<Price, Level> asks;
  };
  using Orders = std::unordered_map<std::uint64_t, Order>;

  void reduce(Orders::iterator it, std::uint32_t shares);

  Orders orders_;
  std::vector<Symbol> symbols_;
  std::size_t peak_orders_ = 0;
  Anomalies anomalies_;
};

}  // namespace book
