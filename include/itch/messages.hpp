#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "itch/endian.hpp"

namespace itch {

// Zero-copy views over Nasdaq TotalView-ITCH 5.0 messages. Fields are decoded from the
// big-endian bytes on access. Prices have four implied decimals except the MWCB levels,
// which have eight. Timestamps are nanoseconds since midnight Eastern time.
class Message {
 public:
  explicit Message(const char* p) noexcept : p_(p) {}

  const char* data() const noexcept { return p_; }
  char type() const noexcept { return p_[0]; }
  std::uint16_t locate() const noexcept { return load_be16(p_ + 1); }
  std::uint16_t tracking() const noexcept { return load_be16(p_ + 3); }
  std::uint64_t timestamp() const noexcept { return load_be48(p_ + 5); }

 protected:
  char ch(std::size_t at) const noexcept { return p_[at]; }
  std::uint32_t u32(std::size_t at) const noexcept { return load_be32(p_ + at); }
  std::uint64_t u64(std::size_t at) const noexcept { return load_be64(p_ + at); }
  std::string_view str(std::size_t at, std::size_t n) const noexcept { return {p_ + at, n}; }

 private:
  const char* p_;
};

struct SystemEvent : Message {
  static constexpr char kType = 'S';
  static constexpr std::size_t kSize = 12;
  using Message::Message;
  char event_code() const noexcept { return ch(11); }
};

struct StockDirectory : Message {
  static constexpr char kType = 'R';
  static constexpr std::size_t kSize = 39;
  using Message::Message;
  std::string_view stock() const noexcept { return str(11, 8); }
  char market_category() const noexcept { return ch(19); }
  char financial_status() const noexcept { return ch(20); }
  std::uint32_t round_lot_size() const noexcept { return u32(21); }
  char round_lots_only() const noexcept { return ch(25); }
  char issue_classification() const noexcept { return ch(26); }
  std::string_view issue_subtype() const noexcept { return str(27, 2); }
  char authenticity() const noexcept { return ch(29); }
  char short_sale_threshold() const noexcept { return ch(30); }
  char ipo_flag() const noexcept { return ch(31); }
  char luld_tier() const noexcept { return ch(32); }
  char etp_flag() const noexcept { return ch(33); }
  std::uint32_t etp_leverage_factor() const noexcept { return u32(34); }
  char inverse_indicator() const noexcept { return ch(38); }
};

struct StockTradingAction : Message {
  static constexpr char kType = 'H';
  static constexpr std::size_t kSize = 25;
  using Message::Message;
  std::string_view stock() const noexcept { return str(11, 8); }
  char trading_state() const noexcept { return ch(19); }
  std::string_view reason() const noexcept { return str(21, 4); }
};

struct RegShoRestriction : Message {
  static constexpr char kType = 'Y';
  static constexpr std::size_t kSize = 20;
  using Message::Message;
  std::string_view stock() const noexcept { return str(11, 8); }
  char action() const noexcept { return ch(19); }
};

struct MarketParticipantPosition : Message {
  static constexpr char kType = 'L';
  static constexpr std::size_t kSize = 26;
  using Message::Message;
  std::string_view mpid() const noexcept { return str(11, 4); }
  std::string_view stock() const noexcept { return str(15, 8); }
  char primary_market_maker() const noexcept { return ch(23); }
  char market_maker_mode() const noexcept { return ch(24); }
  char participant_state() const noexcept { return ch(25); }
};

struct MwcbDeclineLevel : Message {
  static constexpr char kType = 'V';
  static constexpr std::size_t kSize = 35;
  using Message::Message;
  std::uint64_t level1() const noexcept { return u64(11); }
  std::uint64_t level2() const noexcept { return u64(19); }
  std::uint64_t level3() const noexcept { return u64(27); }
};

struct MwcbStatus : Message {
  static constexpr char kType = 'W';
  static constexpr std::size_t kSize = 12;
  using Message::Message;
  char breached_level() const noexcept { return ch(11); }
};

struct IpoQuotingPeriod : Message {
  static constexpr char kType = 'K';
  static constexpr std::size_t kSize = 28;
  using Message::Message;
  std::string_view stock() const noexcept { return str(11, 8); }
  std::uint32_t release_time() const noexcept { return u32(19); }
  char release_qualifier() const noexcept { return ch(23); }
  std::uint32_t ipo_price() const noexcept { return u32(24); }
};

struct LuldAuctionCollar : Message {
  static constexpr char kType = 'J';
  static constexpr std::size_t kSize = 35;
  using Message::Message;
  std::string_view stock() const noexcept { return str(11, 8); }
  std::uint32_t reference_price() const noexcept { return u32(19); }
  std::uint32_t upper_price() const noexcept { return u32(23); }
  std::uint32_t lower_price() const noexcept { return u32(27); }
  std::uint32_t extension() const noexcept { return u32(31); }
};

struct OperationalHalt : Message {
  static constexpr char kType = 'h';
  static constexpr std::size_t kSize = 21;
  using Message::Message;
  std::string_view stock() const noexcept { return str(11, 8); }
  char market_code() const noexcept { return ch(19); }
  char halt_action() const noexcept { return ch(20); }
};

struct AddOrder : Message {
  static constexpr char kType = 'A';
  static constexpr std::size_t kSize = 36;
  using Message::Message;
  std::uint64_t ref() const noexcept { return u64(11); }
  char side() const noexcept { return ch(19); }
  std::uint32_t shares() const noexcept { return u32(20); }
  std::string_view stock() const noexcept { return str(24, 8); }
  std::uint32_t price() const noexcept { return u32(32); }
};

struct AddOrderMpid : Message {
  static constexpr char kType = 'F';
  static constexpr std::size_t kSize = 40;
  using Message::Message;
  std::uint64_t ref() const noexcept { return u64(11); }
  char side() const noexcept { return ch(19); }
  std::uint32_t shares() const noexcept { return u32(20); }
  std::string_view stock() const noexcept { return str(24, 8); }
  std::uint32_t price() const noexcept { return u32(32); }
  std::string_view attribution() const noexcept { return str(36, 4); }
};

struct OrderExecuted : Message {
  static constexpr char kType = 'E';
  static constexpr std::size_t kSize = 31;
  using Message::Message;
  std::uint64_t ref() const noexcept { return u64(11); }
  std::uint32_t shares() const noexcept { return u32(19); }
  std::uint64_t match() const noexcept { return u64(23); }
};

struct OrderExecutedWithPrice : Message {
  static constexpr char kType = 'C';
  static constexpr std::size_t kSize = 36;
  using Message::Message;
  std::uint64_t ref() const noexcept { return u64(11); }
  std::uint32_t shares() const noexcept { return u32(19); }
  std::uint64_t match() const noexcept { return u64(23); }
  char printable() const noexcept { return ch(31); }
  std::uint32_t price() const noexcept { return u32(32); }
};

struct OrderCancel : Message {
  static constexpr char kType = 'X';
  static constexpr std::size_t kSize = 23;
  using Message::Message;
  std::uint64_t ref() const noexcept { return u64(11); }
  std::uint32_t shares() const noexcept { return u32(19); }
};

struct OrderDelete : Message {
  static constexpr char kType = 'D';
  static constexpr std::size_t kSize = 19;
  using Message::Message;
  std::uint64_t ref() const noexcept { return u64(11); }
};

struct OrderReplace : Message {
  static constexpr char kType = 'U';
  static constexpr std::size_t kSize = 35;
  using Message::Message;
  std::uint64_t old_ref() const noexcept { return u64(11); }
  std::uint64_t new_ref() const noexcept { return u64(19); }
  std::uint32_t shares() const noexcept { return u32(27); }
  std::uint32_t price() const noexcept { return u32(31); }
};

struct Trade : Message {
  static constexpr char kType = 'P';
  static constexpr std::size_t kSize = 44;
  using Message::Message;
  std::uint64_t ref() const noexcept { return u64(11); }
  char side() const noexcept { return ch(19); }
  std::uint32_t shares() const noexcept { return u32(20); }
  std::string_view stock() const noexcept { return str(24, 8); }
  std::uint32_t price() const noexcept { return u32(32); }
  std::uint64_t match() const noexcept { return u64(36); }
};

struct CrossTrade : Message {
  static constexpr char kType = 'Q';
  static constexpr std::size_t kSize = 40;
  using Message::Message;
  std::uint64_t shares() const noexcept { return u64(11); }
  std::string_view stock() const noexcept { return str(19, 8); }
  std::uint32_t price() const noexcept { return u32(27); }
  std::uint64_t match() const noexcept { return u64(31); }
  char cross_type() const noexcept { return ch(39); }
};

struct BrokenTrade : Message {
  static constexpr char kType = 'B';
  static constexpr std::size_t kSize = 19;
  using Message::Message;
  std::uint64_t match() const noexcept { return u64(11); }
};

struct Noii : Message {
  static constexpr char kType = 'I';
  static constexpr std::size_t kSize = 50;
  using Message::Message;
  std::uint64_t paired_shares() const noexcept { return u64(11); }
  std::uint64_t imbalance_shares() const noexcept { return u64(19); }
  char imbalance_direction() const noexcept { return ch(27); }
  std::string_view stock() const noexcept { return str(28, 8); }
  std::uint32_t far_price() const noexcept { return u32(36); }
  std::uint32_t near_price() const noexcept { return u32(40); }
  std::uint32_t reference_price() const noexcept { return u32(44); }
  char cross_type() const noexcept { return ch(48); }
  char price_variation() const noexcept { return ch(49); }
};

struct RetailInterest : Message {
  static constexpr char kType = 'N';
  static constexpr std::size_t kSize = 20;
  using Message::Message;
  std::string_view stock() const noexcept { return str(11, 8); }
  char interest_flag() const noexcept { return ch(19); }
};

struct DirectListingPriceDiscovery : Message {
  static constexpr char kType = 'O';
  static constexpr std::size_t kSize = 48;
  using Message::Message;
  std::string_view stock() const noexcept { return str(11, 8); }
  char open_eligibility() const noexcept { return ch(19); }
  std::uint32_t min_price() const noexcept { return u32(20); }
  std::uint32_t max_price() const noexcept { return u32(24); }
  std::uint32_t near_execution_price() const noexcept { return u32(28); }
  std::uint64_t near_execution_time() const noexcept { return u64(32); }
  std::uint32_t lower_collar() const noexcept { return u32(40); }
  std::uint32_t upper_collar() const noexcept { return u32(44); }
};

}  // namespace itch
