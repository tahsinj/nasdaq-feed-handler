#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "itch/messages.hpp"

namespace itch {

// Encodes ITCH 5.0 messages with the two byte length framing of the sample files.
// Used by the synthetic feed generator and the tests.
class Writer {
 public:
  const std::vector<char>& bytes() const noexcept { return out_; }
  std::uint64_t messages() const noexcept { return messages_; }
  void clear() noexcept { out_.clear(); }

  void system_event(std::uint64_t ts, char code) {
    begin(SystemEvent::kType, 0, ts);
    put(code);
    end(SystemEvent::kSize);
  }

  void stock_directory(std::uint16_t locate, std::uint64_t ts, std::string_view stock,
                       std::uint32_t round_lot = 100) {
    begin(StockDirectory::kType, locate, ts);
    text(stock, 8);
    put('Q');
    put('N');
    put32(round_lot);
    put('N');
    put('C');
    text("Z", 2);
    put('P');
    put('N');
    put('N');
    put('1');
    put('N');
    put32(0);
    put('N');
    end(StockDirectory::kSize);
  }

  void trading_action(std::uint16_t locate, std::uint64_t ts, std::string_view stock, char state,
                      std::string_view reason = "") {
    begin(StockTradingAction::kType, locate, ts);
    text(stock, 8);
    put(state);
    put(' ');
    text(reason, 4);
    end(StockTradingAction::kSize);
  }

  void reg_sho(std::uint16_t locate, std::uint64_t ts, std::string_view stock, char action) {
    begin(RegShoRestriction::kType, locate, ts);
    text(stock, 8);
    put(action);
    end(RegShoRestriction::kSize);
  }

  void participant_position(std::uint16_t locate, std::uint64_t ts, std::string_view mpid,
                            std::string_view stock, char primary, char mode, char state) {
    begin(MarketParticipantPosition::kType, locate, ts);
    text(mpid, 4);
    text(stock, 8);
    put(primary);
    put(mode);
    put(state);
    end(MarketParticipantPosition::kSize);
  }

  void mwcb_decline(std::uint64_t ts, std::uint64_t l1, std::uint64_t l2, std::uint64_t l3) {
    begin(MwcbDeclineLevel::kType, 0, ts);
    put64(l1);
    put64(l2);
    put64(l3);
    end(MwcbDeclineLevel::kSize);
  }

  void mwcb_status(std::uint64_t ts, char level) {
    begin(MwcbStatus::kType, 0, ts);
    put(level);
    end(MwcbStatus::kSize);
  }

  void ipo_quoting(std::uint16_t locate, std::uint64_t ts, std::string_view stock,
                   std::uint32_t release_time, char qualifier, std::uint32_t price) {
    begin(IpoQuotingPeriod::kType, locate, ts);
    text(stock, 8);
    put32(release_time);
    put(qualifier);
    put32(price);
    end(IpoQuotingPeriod::kSize);
  }

  void luld_collar(std::uint16_t locate, std::uint64_t ts, std::string_view stock,
                   std::uint32_t reference, std::uint32_t upper, std::uint32_t lower,
                   std::uint32_t extension) {
    begin(LuldAuctionCollar::kType, locate, ts);
    text(stock, 8);
    put32(reference);
    put32(upper);
    put32(lower);
    put32(extension);
    end(LuldAuctionCollar::kSize);
  }

  void operational_halt(std::uint16_t locate, std::uint64_t ts, std::string_view stock, char market,
                        char action) {
    begin(OperationalHalt::kType, locate, ts);
    text(stock, 8);
    put(market);
    put(action);
    end(OperationalHalt::kSize);
  }

  void add_order(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, char side,
                 std::uint32_t shares, std::string_view stock, std::uint32_t price) {
    begin(AddOrder::kType, locate, ts);
    put64(ref);
    put(side);
    put32(shares);
    text(stock, 8);
    put32(price);
    end(AddOrder::kSize);
  }

  void add_order_mpid(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, char side,
                      std::uint32_t shares, std::string_view stock, std::uint32_t price,
                      std::string_view mpid) {
    begin(AddOrderMpid::kType, locate, ts);
    put64(ref);
    put(side);
    put32(shares);
    text(stock, 8);
    put32(price);
    text(mpid, 4);
    end(AddOrderMpid::kSize);
  }

  void executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares,
                std::uint64_t match) {
    begin(OrderExecuted::kType, locate, ts);
    put64(ref);
    put32(shares);
    put64(match);
    end(OrderExecuted::kSize);
  }

  void executed_with_price(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                           std::uint32_t shares, std::uint64_t match, char printable,
                           std::uint32_t price) {
    begin(OrderExecutedWithPrice::kType, locate, ts);
    put64(ref);
    put32(shares);
    put64(match);
    put(printable);
    put32(price);
    end(OrderExecutedWithPrice::kSize);
  }

  void cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
    begin(OrderCancel::kType, locate, ts);
    put64(ref);
    put32(shares);
    end(OrderCancel::kSize);
  }

  void order_delete(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
    begin(OrderDelete::kType, locate, ts);
    put64(ref);
    end(OrderDelete::kSize);
  }

  void replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t old_ref, std::uint64_t new_ref,
               std::uint32_t shares, std::uint32_t price) {
    begin(OrderReplace::kType, locate, ts);
    put64(old_ref);
    put64(new_ref);
    put32(shares);
    put32(price);
    end(OrderReplace::kSize);
  }

  void trade(std::uint16_t locate, std::uint64_t ts, char side, std::uint32_t shares,
             std::string_view stock, std::uint32_t price, std::uint64_t match) {
    begin(Trade::kType, locate, ts);
    put64(0);
    put(side);
    put32(shares);
    text(stock, 8);
    put32(price);
    put64(match);
    end(Trade::kSize);
  }

  void cross(std::uint16_t locate, std::uint64_t ts, std::uint64_t shares, std::string_view stock,
             std::uint32_t price, std::uint64_t match, char cross_type) {
    begin(CrossTrade::kType, locate, ts);
    put64(shares);
    text(stock, 8);
    put32(price);
    put64(match);
    put(cross_type);
    end(CrossTrade::kSize);
  }

  void broken(std::uint16_t locate, std::uint64_t ts, std::uint64_t match) {
    begin(BrokenTrade::kType, locate, ts);
    put64(match);
    end(BrokenTrade::kSize);
  }

  void noii(std::uint16_t locate, std::uint64_t ts, std::uint64_t paired, std::uint64_t imbalance,
            char direction, std::string_view stock, std::uint32_t far_price,
            std::uint32_t near_price, std::uint32_t reference, char cross_type, char variation) {
    begin(Noii::kType, locate, ts);
    put64(paired);
    put64(imbalance);
    put(direction);
    text(stock, 8);
    put32(far_price);
    put32(near_price);
    put32(reference);
    put(cross_type);
    put(variation);
    end(Noii::kSize);
  }

  void retail_interest(std::uint16_t locate, std::uint64_t ts, std::string_view stock, char flag) {
    begin(RetailInterest::kType, locate, ts);
    text(stock, 8);
    put(flag);
    end(RetailInterest::kSize);
  }

  void direct_listing(std::uint16_t locate, std::uint64_t ts, std::string_view stock,
                      char eligibility, std::uint32_t min_price, std::uint32_t max_price,
                      std::uint32_t near_price, std::uint64_t near_time, std::uint32_t lower,
                      std::uint32_t upper) {
    begin(DirectListingPriceDiscovery::kType, locate, ts);
    text(stock, 8);
    put(eligibility);
    put32(min_price);
    put32(max_price);
    put32(near_price);
    put64(near_time);
    put32(lower);
    put32(upper);
    end(DirectListingPriceDiscovery::kSize);
  }

 private:
  void begin(char type, std::uint16_t locate, std::uint64_t ts) {
    start_ = out_.size();
    put16(0);
    put(type);
    put16(locate);
    put16(0);
    put48(ts);
  }

  void end(std::size_t size) {
    const std::size_t written = out_.size() - start_ - 2;
    if (written != size) throw std::logic_error("ITCH writer produced a message of the wrong size");
    out_[start_] = static_cast<char>(size >> 8);
    out_[start_ + 1] = static_cast<char>(size & 0xff);
    ++messages_;
  }

  void put(char c) { out_.push_back(c); }
  void put16(std::uint16_t v) {
    put(static_cast<char>(v >> 8));
    put(static_cast<char>(v & 0xff));
  }
  void put32(std::uint32_t v) {
    put16(static_cast<std::uint16_t>(v >> 16));
    put16(static_cast<std::uint16_t>(v & 0xffff));
  }
  void put48(std::uint64_t v) {
    put16(static_cast<std::uint16_t>((v >> 32) & 0xffff));
    put32(static_cast<std::uint32_t>(v & 0xffffffff));
  }
  void put64(std::uint64_t v) {
    put32(static_cast<std::uint32_t>(v >> 32));
    put32(static_cast<std::uint32_t>(v & 0xffffffff));
  }
  void text(std::string_view s, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) put(i < s.size() ? s[i] : ' ');
  }

  std::vector<char> out_;
  std::size_t start_ = 0;
  std::uint64_t messages_ = 0;
};

}  // namespace itch
