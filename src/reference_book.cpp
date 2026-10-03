#include "book/reference_book.hpp"

#include <algorithm>

namespace book {

ReferenceBook::ReferenceBook() : symbols_(65536) {}

void ReferenceBook::add(std::uint16_t locate, std::uint64_t ref, Side side, std::uint32_t shares,
                        Price price) {
  if (shares == 0) {
    ++anomalies_.zero_shares;
    return;
  }
  if (!orders_.try_emplace(ref, Order{locate, side, price, shares}).second) {
    ++anomalies_.duplicate_order;
    return;
  }
  peak_orders_ = std::max(peak_orders_, orders_.size());
  Symbol& s = symbols_[locate];
  Level& level = side == Side::Buy ? s.bids[price] : s.asks[price];
  level.qty += shares;
  ++level.orders;
}

void ReferenceBook::reduce(Orders::iterator it, std::uint32_t shares) {
  Order& order = it->second;
  if (shares > order.shares) {
    ++anomalies_.overfill;
    shares = order.shares;
  }
  Symbol& s = symbols_[order.locate];
  auto apply = [&](auto& levels) {
    const auto level = levels.find(order.price);
    level->second.qty -= shares;
    order.shares -= shares;
    if (order.shares != 0) return;
    if (--level->second.orders == 0) levels.erase(level);
    orders_.erase(it);
  };
  if (order.side == Side::Buy) {
    apply(s.bids);
  } else {
    apply(s.asks);
  }
}

void ReferenceBook::execute(std::uint64_t ref, std::uint32_t shares) {
  const auto it = orders_.find(ref);
  if (it == orders_.end()) {
    ++anomalies_.unknown_order;
    return;
  }
  reduce(it, shares);
}

void ReferenceBook::cancel(std::uint64_t ref, std::uint32_t shares) { execute(ref, shares); }

void ReferenceBook::remove(std::uint64_t ref) {
  const auto it = orders_.find(ref);
  if (it == orders_.end()) {
    ++anomalies_.unknown_order;
    return;
  }
  reduce(it, it->second.shares);
}

void ReferenceBook::replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares,
                            Price price) {
  const auto it = orders_.find(old_ref);
  if (it == orders_.end()) {
    ++anomalies_.unknown_order;
    return;
  }
  const Order old = it->second;
  reduce(it, old.shares);
  add(old.locate, new_ref, old.side, shares, price);
}

TopOfBook ReferenceBook::top(std::uint16_t locate) const {
  TopOfBook t;
  const Symbol& s = symbols_[locate];
  if (!s.bids.empty()) {
    const auto& [price, level] = *s.bids.begin();
    t.bid = {price, level.orders, level.qty};
  }
  if (!s.asks.empty()) {
    const auto& [price, level] = *s.asks.begin();
    t.ask = {price, level.orders, level.qty};
  }
  return t;
}

}  // namespace book
