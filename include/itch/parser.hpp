#pragma once

#include <cstddef>

#include "itch/endian.hpp"
#include "itch/messages.hpp"

namespace itch {

// Calls fn(message, length) for every complete message. The sample files frame each message
// with a two byte big-endian length. Returns the bytes consumed; anything short of size is a
// truncated tail.
template <class Fn>
std::size_t for_each_message(const char* data, std::size_t size, Fn&& fn) {
  std::size_t pos = 0;
  while (size - pos >= 2) {
    const std::size_t len = load_be16(data + pos);
    if (len == 0 || size - pos - 2 < len) break;
    fn(data + pos + 2, len);
    pos += 2 + len;
  }
  return pos;
}

namespace detail {

template <class M, class Handler>
inline void deliver(const char* msg, std::size_t len, Handler& h) {
  if constexpr (requires { h.on(M{msg}); }) {
    if (len >= M::kSize) [[likely]] {
      h.on(M{msg});
    } else if constexpr (requires { h.on_malformed(msg, len); }) {
      h.on_malformed(msg, len);
    }
  }
}

}  // namespace detail

// Calls handler.on(view) for message types the handler has an overload for. Optional hooks:
// on_malformed(msg, len) for messages shorter than the spec and on_unknown(msg, len).
template <class Handler>
inline void dispatch(const char* msg, std::size_t len, Handler& h) {
  switch (msg[0]) {
    case AddOrder::kType:
      detail::deliver<AddOrder>(msg, len, h);
      break;
    case AddOrderMpid::kType:
      detail::deliver<AddOrderMpid>(msg, len, h);
      break;
    case OrderExecuted::kType:
      detail::deliver<OrderExecuted>(msg, len, h);
      break;
    case OrderExecutedWithPrice::kType:
      detail::deliver<OrderExecutedWithPrice>(msg, len, h);
      break;
    case OrderCancel::kType:
      detail::deliver<OrderCancel>(msg, len, h);
      break;
    case OrderDelete::kType:
      detail::deliver<OrderDelete>(msg, len, h);
      break;
    case OrderReplace::kType:
      detail::deliver<OrderReplace>(msg, len, h);
      break;
    case Trade::kType:
      detail::deliver<Trade>(msg, len, h);
      break;
    case CrossTrade::kType:
      detail::deliver<CrossTrade>(msg, len, h);
      break;
    case BrokenTrade::kType:
      detail::deliver<BrokenTrade>(msg, len, h);
      break;
    case Noii::kType:
      detail::deliver<Noii>(msg, len, h);
      break;
    case SystemEvent::kType:
      detail::deliver<SystemEvent>(msg, len, h);
      break;
    case StockDirectory::kType:
      detail::deliver<StockDirectory>(msg, len, h);
      break;
    case StockTradingAction::kType:
      detail::deliver<StockTradingAction>(msg, len, h);
      break;
    case RegShoRestriction::kType:
      detail::deliver<RegShoRestriction>(msg, len, h);
      break;
    case MarketParticipantPosition::kType:
      detail::deliver<MarketParticipantPosition>(msg, len, h);
      break;
    case MwcbDeclineLevel::kType:
      detail::deliver<MwcbDeclineLevel>(msg, len, h);
      break;
    case MwcbStatus::kType:
      detail::deliver<MwcbStatus>(msg, len, h);
      break;
    case IpoQuotingPeriod::kType:
      detail::deliver<IpoQuotingPeriod>(msg, len, h);
      break;
    case LuldAuctionCollar::kType:
      detail::deliver<LuldAuctionCollar>(msg, len, h);
      break;
    case OperationalHalt::kType:
      detail::deliver<OperationalHalt>(msg, len, h);
      break;
    case RetailInterest::kType:
      detail::deliver<RetailInterest>(msg, len, h);
      break;
    case DirectListingPriceDiscovery::kType:
      detail::deliver<DirectListingPriceDiscovery>(msg, len, h);
      break;
    default:
      if constexpr (requires { h.on_unknown(msg, len); }) h.on_unknown(msg, len);
      break;
  }
}

template <class Handler>
std::size_t parse(const char* data, std::size_t size, Handler& h) {
  return for_each_message(data, size,
                          [&h](const char* msg, std::size_t len) { dispatch(msg, len, h); });
}

}  // namespace itch
