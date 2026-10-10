#include "itch/summary.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>

#include "itch/messages.hpp"
#include "itch/parser.hpp"

namespace itch {

std::size_t spec_size(char type) noexcept {
  switch (type) {
    case SystemEvent::kType:
      return SystemEvent::kSize;
    case StockDirectory::kType:
      return StockDirectory::kSize;
    case StockTradingAction::kType:
      return StockTradingAction::kSize;
    case RegShoRestriction::kType:
      return RegShoRestriction::kSize;
    case MarketParticipantPosition::kType:
      return MarketParticipantPosition::kSize;
    case MwcbDeclineLevel::kType:
      return MwcbDeclineLevel::kSize;
    case MwcbStatus::kType:
      return MwcbStatus::kSize;
    case IpoQuotingPeriod::kType:
      return IpoQuotingPeriod::kSize;
    case LuldAuctionCollar::kType:
      return LuldAuctionCollar::kSize;
    case OperationalHalt::kType:
      return OperationalHalt::kSize;
    case AddOrder::kType:
      return AddOrder::kSize;
    case AddOrderMpid::kType:
      return AddOrderMpid::kSize;
    case OrderExecuted::kType:
      return OrderExecuted::kSize;
    case OrderExecutedWithPrice::kType:
      return OrderExecutedWithPrice::kSize;
    case OrderCancel::kType:
      return OrderCancel::kSize;
    case OrderDelete::kType:
      return OrderDelete::kSize;
    case OrderReplace::kType:
      return OrderReplace::kSize;
    case Trade::kType:
      return Trade::kSize;
    case CrossTrade::kType:
      return CrossTrade::kSize;
    case BrokenTrade::kType:
      return BrokenTrade::kSize;
    case Noii::kType:
      return Noii::kSize;
    case RetailInterest::kType:
      return RetailInterest::kSize;
    case DirectListingPriceDiscovery::kType:
      return DirectListingPriceDiscovery::kSize;
    default:
      return 0;
  }
}

const char* message_name(char type) noexcept {
  switch (type) {
    case SystemEvent::kType:
      return "system event";
    case StockDirectory::kType:
      return "stock directory";
    case StockTradingAction::kType:
      return "trading action";
    case RegShoRestriction::kType:
      return "reg sho";
    case MarketParticipantPosition::kType:
      return "mp position";
    case MwcbDeclineLevel::kType:
      return "mwcb level";
    case MwcbStatus::kType:
      return "mwcb status";
    case IpoQuotingPeriod::kType:
      return "ipo quoting";
    case LuldAuctionCollar::kType:
      return "luld collar";
    case OperationalHalt::kType:
      return "operational halt";
    case AddOrder::kType:
      return "add";
    case AddOrderMpid::kType:
      return "add mpid";
    case OrderExecuted::kType:
      return "executed";
    case OrderExecutedWithPrice::kType:
      return "executed price";
    case OrderCancel::kType:
      return "cancel";
    case OrderDelete::kType:
      return "delete";
    case OrderReplace::kType:
      return "replace";
    case Trade::kType:
      return "trade";
    case CrossTrade::kType:
      return "cross trade";
    case BrokenTrade::kType:
      return "broken trade";
    case Noii::kType:
      return "noii";
    case RetailInterest::kType:
      return "rpii";
    case DirectListingPriceDiscovery::kType:
      return "dlcr";
    default:
      return "unknown";
  }
}

FileSummary summarize(const char* data, std::size_t size) {
  if (size >= 2 && static_cast<unsigned char>(data[0]) == 0x1f &&
      static_cast<unsigned char>(data[1]) == 0x8b) {
    throw std::runtime_error("input is gzip-compressed; decompress it first");
  }
  FileSummary s;
  s.bytes = for_each_message(data, size, [&s](const char* msg, std::size_t len) {
    ++s.messages;
    ++s.count[static_cast<unsigned char>(msg[0])];
    const std::size_t want = spec_size(msg[0]);
    if (want == 0) return;
    if (len < want) {
      ++s.malformed;
      return;
    }
    const std::uint16_t locate = load_be16(msg + 1);
    s.max_locate = std::max<std::uint32_t>(s.max_locate, locate);
    if (msg[0] == StockDirectory::kType) {
      std::string_view name = StockDirectory{msg}.stock();
      while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
      if (s.symbols.size() <= locate) s.symbols.resize(std::size_t{locate} + 1);
      s.symbols[locate] = std::string(name);
    }
  });
  return s;
}

}  // namespace itch
