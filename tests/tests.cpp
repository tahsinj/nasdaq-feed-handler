#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <vector>

#include "book/fast_book.hpp"
#include "book/feed.hpp"
#include "book/order_map.hpp"
#include "book/reference_book.hpp"
#include "itch/parser.hpp"
#include "itch/summary.hpp"
#include "itch/writer.hpp"
#include "util/alloc_counter.hpp"
#include "util/histogram.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const char* expr, const char* file, int line) {
  if (ok) return;
  ++g_failures;
  std::printf("  %s:%d: check failed: %s\n", file, line, expr);
}

#define CHECK(expr) check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

struct Test {
  const char* name;
  void (*fn)();
};

std::vector<Test>& registry() {
  static std::vector<Test> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

#define TEST(name)                               \
  void name();                                   \
  const Registrar name##_registrar{#name, name}; \
  void name()

using book::Price;
using book::Side;
using book::TopOfBook;

constexpr Price px(double dollars) { return static_cast<Price>(dollars * 10000.0 + 0.5); }

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  std::uint64_t below(std::uint64_t n) { return next() % n; }
  double unit() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
  bool chance(double p) { return unit() < p; }

 private:
  std::uint64_t state_;
};

book::FastBookConfig small_config() {
  book::FastBookConfig c;
  c.num_locates = 16;
  c.max_orders = 1u << 18;
  c.far_arena_bytes = std::size_t{32} << 20;
  return c;
}

template <class Fn>
void with_both_books(Fn&& fn) {
  {
    book::ReferenceBook b;
    fn(b);
  }
  {
    book::FastBook b(small_config());
    fn(b);
  }
}

TEST(big_endian_loads) {
  const unsigned char raw[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  const char* p = reinterpret_cast<const char*>(raw);
  CHECK(itch::load_be16(p) == 0x0102);
  CHECK(itch::load_be32(p) == 0x01020304u);
  CHECK(itch::load_be48(p) == 0x010203040506ull);
  CHECK(itch::load_be64(p) == 0x0102030405060708ull);
}

TEST(add_order_fields_sit_at_spec_offsets) {
  const unsigned char raw[36] = {
      'A',                                             // 0 message type
      0x00, 0x2a,                                      // 1 stock locate 42
      0x00, 0x07,                                      // 3 tracking number 7
      0x00, 0x00, 0x1f, 0x5a, 0x6b, 0x7c,              // 5 timestamp
      0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xe2, 0x40,  // 11 order reference 123456
      'S',                                             // 19 side
      0x00, 0x00, 0x01, 0x2c,                          // 20 shares 300
      'A',  'A',  'P',  'L',  ' ',  ' ',  ' ',  ' ',   // 24 stock
      0x00, 0x1b, 0x3a, 0x0c,                          // 32 price 178.4332
  };
  const itch::AddOrder m{reinterpret_cast<const char*>(raw)};
  CHECK(m.type() == 'A');
  CHECK(m.locate() == 42);
  CHECK(m.tracking() == 7);
  CHECK(m.timestamp() == 0x1f5a6b7cull);
  CHECK(m.ref() == 123456);
  CHECK(m.side() == 'S');
  CHECK(m.shares() == 300);
  CHECK(m.stock() == "AAPL    ");
  CHECK(m.price() == 1784332);
}

TEST(message_sizes_match_spec) {
  const std::pair<char, std::size_t> sizes[] = {
      {'S', 12}, {'R', 39}, {'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35}, {'W', 12}, {'K', 28},
      {'J', 35}, {'h', 21}, {'A', 36}, {'F', 40}, {'E', 31}, {'C', 36}, {'X', 23}, {'D', 19},
      {'U', 35}, {'P', 44}, {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48}};
  for (const auto& [type, size] : sizes) CHECK(itch::spec_size(type) == size);
  CHECK(itch::spec_size('Z') == 0);
}

constexpr std::uint64_t kTs = 34'200'000'000'123;

struct RoundTrip {
  int seen = 0;

  void common(const itch::Message& m, std::uint16_t locate) {
    ++seen;
    CHECK(m.locate() == locate);
    CHECK(m.timestamp() == kTs);
  }

  void on(const itch::SystemEvent& m) {
    common(m, 0);
    CHECK(m.event_code() == 'Q');
  }
  void on(const itch::StockDirectory& m) {
    common(m, 9);
    CHECK(m.stock() == "MSFT    ");
    CHECK(m.market_category() == 'Q');
    CHECK(m.round_lot_size() == 100);
    CHECK(m.inverse_indicator() == 'N');
  }
  void on(const itch::StockTradingAction& m) {
    common(m, 9);
    CHECK(m.trading_state() == 'H');
    CHECK(m.reason() == "T1  ");
  }
  void on(const itch::RegShoRestriction& m) {
    common(m, 9);
    CHECK(m.action() == '1');
  }
  void on(const itch::MarketParticipantPosition& m) {
    common(m, 9);
    CHECK(m.mpid() == "GSCO");
    CHECK(m.stock() == "MSFT    ");
    CHECK(m.participant_state() == 'A');
  }
  void on(const itch::MwcbDeclineLevel& m) {
    common(m, 0);
    CHECK(m.level1() == 1);
    CHECK(m.level2() == 2);
    CHECK(m.level3() == 3);
  }
  void on(const itch::MwcbStatus& m) {
    common(m, 0);
    CHECK(m.breached_level() == '2');
  }
  void on(const itch::IpoQuotingPeriod& m) {
    common(m, 9);
    CHECK(m.release_time() == 34200);
    CHECK(m.release_qualifier() == 'A');
    CHECK(m.ipo_price() == 250000);
  }
  void on(const itch::LuldAuctionCollar& m) {
    common(m, 9);
    CHECK(m.reference_price() == 100);
    CHECK(m.upper_price() == 110);
    CHECK(m.lower_price() == 90);
    CHECK(m.extension() == 2);
  }
  void on(const itch::OperationalHalt& m) {
    common(m, 9);
    CHECK(m.market_code() == 'Q');
    CHECK(m.halt_action() == 'H');
  }
  void on(const itch::AddOrder& m) {
    common(m, 9);
    CHECK(m.ref() == 77);
    CHECK(m.side() == 'B');
    CHECK(m.shares() == 500);
    CHECK(m.stock() == "MSFT    ");
    CHECK(m.price() == 3'000'100);
  }
  void on(const itch::AddOrderMpid& m) {
    common(m, 9);
    CHECK(m.ref() == 78);
    CHECK(m.side() == 'S');
    CHECK(m.attribution() == "UBSS");
  }
  void on(const itch::OrderExecuted& m) {
    common(m, 9);
    CHECK(m.ref() == 77);
    CHECK(m.shares() == 100);
    CHECK(m.match() == 5);
  }
  void on(const itch::OrderExecutedWithPrice& m) {
    common(m, 9);
    CHECK(m.match() == 6);
    CHECK(m.printable() == 'N');
    CHECK(m.price() == 3'000'000);
  }
  void on(const itch::OrderCancel& m) {
    common(m, 9);
    CHECK(m.shares() == 50);
  }
  void on(const itch::OrderDelete& m) {
    common(m, 9);
    CHECK(m.ref() == 78);
  }
  void on(const itch::OrderReplace& m) {
    common(m, 9);
    CHECK(m.old_ref() == 77);
    CHECK(m.new_ref() == 79);
    CHECK(m.shares() == 200);
    CHECK(m.price() == 2'999'900);
  }
  void on(const itch::Trade& m) {
    common(m, 9);
    CHECK(m.ref() == 0);
    CHECK(m.side() == 'S');
    CHECK(m.stock() == "MSFT    ");
    CHECK(m.match() == 8);
  }
  void on(const itch::CrossTrade& m) {
    common(m, 9);
    CHECK(m.shares() == 5'000'000'000ull);
    CHECK(m.match() == 9);
    CHECK(m.cross_type() == 'C');
  }
  void on(const itch::BrokenTrade& m) {
    common(m, 9);
    CHECK(m.match() == 8);
  }
  void on(const itch::Noii& m) {
    common(m, 9);
    CHECK(m.paired_shares() == 1000);
    CHECK(m.imbalance_shares() == 200);
    CHECK(m.imbalance_direction() == 'S');
    CHECK(m.stock() == "MSFT    ");
    CHECK(m.far_price() == 1);
    CHECK(m.near_price() == 2);
    CHECK(m.reference_price() == 3);
    CHECK(m.cross_type() == 'O');
    CHECK(m.price_variation() == 'A');
  }
  void on(const itch::RetailInterest& m) {
    common(m, 9);
    CHECK(m.interest_flag() == 'S');
  }
  void on(const itch::DirectListingPriceDiscovery& m) {
    common(m, 9);
    CHECK(m.open_eligibility() == 'Y');
    CHECK(m.min_price() == 10);
    CHECK(m.max_price() == 20);
    CHECK(m.near_execution_price() == 15);
    CHECK(m.near_execution_time() == 99);
    CHECK(m.lower_collar() == 12);
    CHECK(m.upper_collar() == 18);
  }
};

TEST(every_message_type_round_trips) {
  itch::Writer w;
  w.system_event(kTs, 'Q');
  w.stock_directory(9, kTs, "MSFT");
  w.trading_action(9, kTs, "MSFT", 'H', "T1");
  w.reg_sho(9, kTs, "MSFT", '1');
  w.participant_position(9, kTs, "GSCO", "MSFT", 'Y', 'N', 'A');
  w.mwcb_decline(kTs, 1, 2, 3);
  w.mwcb_status(kTs, '2');
  w.ipo_quoting(9, kTs, "MSFT", 34200, 'A', 250000);
  w.luld_collar(9, kTs, "MSFT", 100, 110, 90, 2);
  w.operational_halt(9, kTs, "MSFT", 'Q', 'H');
  w.add_order(9, kTs, 77, 'B', 500, "MSFT", 3'000'100);
  w.add_order_mpid(9, kTs, 78, 'S', 300, "MSFT", 3'000'500, "UBSS");
  w.executed(9, kTs, 77, 100, 5);
  w.executed_with_price(9, kTs, 77, 100, 6, 'N', 3'000'000);
  w.cancel(9, kTs, 77, 50);
  w.order_delete(9, kTs, 78);
  w.replace(9, kTs, 77, 79, 200, 2'999'900);
  w.trade(9, kTs, 'S', 100, "MSFT", 3'000'000, 8);
  w.cross(9, kTs, 5'000'000'000ull, "MSFT", 3'000'000, 9, 'C');
  w.broken(9, kTs, 8);
  w.noii(9, kTs, 1000, 200, 'S', "MSFT", 1, 2, 3, 'O', 'A');
  w.retail_interest(9, kTs, "MSFT", 'S');
  w.direct_listing(9, kTs, "MSFT", 'Y', 10, 20, 15, 99, 12, 18);

  RoundTrip h;
  CHECK(itch::parse(w.bytes().data(), w.bytes().size(), h) == w.bytes().size());
  CHECK(h.seen == 23);

  const itch::FileSummary s = itch::summarize(w.bytes().data(), w.bytes().size());
  CHECK(s.messages == 23);
  CHECK(s.malformed == 0);
  CHECK(s.max_locate == 9);
  CHECK(s.symbols.size() == 10 && s.symbols[9] == "MSFT");
}

TEST(framing_stops_before_a_truncated_message) {
  itch::Writer w;
  w.system_event(1, 'O');
  w.order_delete(1, 2, 3);
  int count = 0;
  const std::size_t used = itch::for_each_message(w.bytes().data(), w.bytes().size() - 1,
                                                  [&](const char*, std::size_t) { ++count; });
  CHECK(count == 1);
  CHECK(used == 2 + itch::SystemEvent::kSize);
}

TEST(short_messages_are_reported_not_decoded) {
  const char raw[] = {0x00, 0x05, 'A', 0x00, 0x01, 0x00, 0x00};
  struct Handler {
    int adds = 0;
    int malformed = 0;
    void on(const itch::AddOrder&) { ++adds; }
    void on_malformed(const char*, std::size_t) { ++malformed; }
  } h;
  itch::parse(raw, sizeof raw, h);
  CHECK(h.adds == 0);
  CHECK(h.malformed == 1);
}

TEST(orders_at_one_price_aggregate) {
  with_both_books([](auto& b) {
    b.add(1, 10, Side::Buy, 100, px(10.00));
    b.add(1, 11, Side::Buy, 200, px(10.00));
    b.add(1, 12, Side::Buy, 300, px(9.99));
    b.add(1, 13, Side::Sell, 50, px(10.02));
    const TopOfBook t = b.top(1);
    CHECK(t.bid.price == px(10.00));
    CHECK(t.bid.qty == 300);
    CHECK(t.bid.orders == 2);
    CHECK(t.ask.price == px(10.02));
    CHECK(t.ask.qty == 50);
    CHECK(b.top(2) == TopOfBook{});
  });
}

TEST(executions_cancels_and_deletes_reduce_the_book) {
  with_both_books([](auto& b) {
    b.add(1, 1, Side::Sell, 300, px(20.05));
    b.add(1, 2, Side::Sell, 100, px(20.06));
    b.execute(1, 100);
    CHECK(b.top(1).ask.qty == 200);
    b.cancel(1, 50);
    CHECK(b.top(1).ask.qty == 150);
    b.execute(1, 150);
    CHECK(b.top(1).ask.price == px(20.06));
    b.remove(2);
    CHECK(b.top(1).ask.qty == 0);
    CHECK(b.live_orders() == 0);
    CHECK(b.anomalies().total() == 0);
  });
}

TEST(replace_keeps_side_and_symbol) {
  with_both_books([](auto& b) {
    b.add(3, 1, Side::Buy, 100, px(50.00));
    b.replace(1, 2, 400, px(50.10));
    const TopOfBook t = b.top(3);
    CHECK(t.bid.price == px(50.10));
    CHECK(t.bid.qty == 400);
    CHECK(t.ask.qty == 0);
    b.execute(1, 10);
    CHECK(b.anomalies().unknown_order == 1);
    b.remove(2);
    CHECK(b.top(3) == TopOfBook{});
  });
}

TEST(bad_feed_events_are_counted_and_ignored) {
  with_both_books([](auto& b) {
    b.add(1, 1, Side::Buy, 100, px(5.00));
    b.add(1, 1, Side::Buy, 100, px(5.01));
    b.add(1, 2, Side::Buy, 0, px(5.02));
    b.execute(99, 10);
    b.remove(98);
    b.replace(97, 3, 10, px(5.03));
    b.execute(1, 150);
    CHECK(b.anomalies().duplicate_order == 1);
    CHECK(b.anomalies().zero_shares == 1);
    CHECK(b.anomalies().unknown_order == 3);
    CHECK(b.anomalies().overfill == 1);
    CHECK(b.top(1) == TopOfBook{});
    CHECK(b.live_orders() == 0);
  });
}

TEST(sub_dollar_ticks_and_the_dollar_boundary) {
  with_both_books([](auto& b) {
    b.add(1, 1, Side::Buy, 1000, 9999);
    b.add(1, 2, Side::Buy, 1000, 9998);
    b.add(1, 3, Side::Sell, 1000, 10000);
    b.add(1, 4, Side::Sell, 1000, 10100);
    CHECK(b.top(1).bid.price == 9999);
    CHECK(b.top(1).ask.price == 10000);
    b.remove(1);
    b.remove(3);
    CHECK(b.top(1).bid.price == 9998);
    CHECK(b.top(1).ask.price == 10100);
  });
}

TEST(far_levels_and_window_moves) {
  with_both_books([](auto& b) {
    b.add(1, 1, Side::Buy, 100, px(100.00));
    b.add(1, 2, Side::Buy, 200, px(40.00));
    b.add(1, 3, Side::Sell, 300, px(100.05));
    b.add(1, 4, Side::Sell, 400, px(190.00));
    CHECK(b.top(1).bid.price == px(100.00));
    b.remove(1);
    CHECK(b.top(1).bid.price == px(40.00));
    CHECK(b.top(1).bid.qty == 200);
    b.remove(3);
    CHECK(b.top(1).ask.price == px(190.00));
    b.add(1, 5, Side::Buy, 500, px(185.00));
    CHECK(b.top(1).bid.price == px(185.00));
    b.remove(5);
    CHECK(b.top(1).bid.price == px(40.00));
    b.add(1, 6, Side::Sell, 600, px(41.00));
    CHECK(b.top(1).ask.price == px(41.00));
    b.remove(6);
    CHECK(b.top(1).ask.price == px(190.00));
    b.remove(2);
    b.remove(4);
    CHECK(b.top(1) == TopOfBook{});
  });
}

TEST(prices_off_the_cent_grid) {
  with_both_books([](auto& b) {
    b.add(1, 1, Side::Buy, 100, px(10.00));
    b.add(1, 2, Side::Buy, 100, 100050);
    b.add(1, 3, Side::Sell, 100, 100150);
    b.add(1, 4, Side::Sell, 100, px(10.02));
    CHECK(b.top(1).bid.price == 100050);
    CHECK(b.top(1).ask.price == 100150);
    b.remove(2);
    b.remove(3);
    CHECK(b.top(1).bid.price == px(10.00));
    CHECK(b.top(1).ask.price == px(10.02));
  });
}

TEST(trades_crosses_and_broken_trades_leave_the_book_alone) {
  itch::Writer w;
  w.add_order(1, 1, 10, 'B', 500, "TEST", px(25.00));
  w.add_order(1, 2, 11, 'S', 300, "TEST", px(25.02));
  w.executed_with_price(1, 3, 10, 200, 1, 'Y', px(24.99));
  w.trade(1, 4, 'B', 1000, "TEST", px(25.01), 2);
  w.cross(1, 5, 50000, "TEST", px(25.01), 3, 'O');
  w.broken(1, 6, 2);
  w.replace(1, 7, 11, 12, 100, px(25.03));
  w.cancel(1, 8, 12, 40);

  book::FastBook fast(small_config());
  book::ReferenceBook ref;
  book::FeedHandler<book::FastBook> fast_feed(fast);
  book::FeedHandler<book::ReferenceBook> ref_feed(ref);
  itch::parse(w.bytes().data(), w.bytes().size(), fast_feed);
  itch::parse(w.bytes().data(), w.bytes().size(), ref_feed);
  for (const TopOfBook& t : {fast.top(1), ref.top(1)}) {
    CHECK(t.bid.price == px(25.00));
    CHECK(t.bid.qty == 300);
    CHECK(t.ask.price == px(25.03));
    CHECK(t.ask.qty == 60);
  }
  CHECK(fast.anomalies().total() == 0);
  CHECK(ref.anomalies().total() == 0);
}

TEST(fast_book_matches_reference_on_random_flow) {
  struct Live {
    std::uint64_t ref;
    std::uint16_t locate;
    std::uint32_t shares;
    Side side;
  };
  const std::int64_t tick[9] = {0, 100, 100, 1, 100, 100, 100, 1, 100};
  std::int64_t mid[9] = {0, 250'000, 1'200'000, 9'000, 30'000'000, 99'500, 4'500'000, 500, 10'000};

  book::FastBook fast(small_config());
  book::ReferenceBook ref;
  Rng rng(2024);
  std::vector<Live> live;
  std::uint64_t next_ref = 1;

  const auto quote = [&](std::uint16_t locate, Side side) {
    std::int64_t d = 1;
    const double q = rng.unit();
    if (q < 0.85) {
      while (d < 40 && rng.chance(0.6)) ++d;
    } else if (q < 0.97) {
      d = 40 + static_cast<std::int64_t>(rng.below(3000));
    } else {
      d = 3000 + static_cast<std::int64_t>(rng.below(20000));
    }
    std::int64_t p =
        side == Side::Buy ? mid[locate] - d * tick[locate] : mid[locate] + d * tick[locate];
    if (tick[locate] == 100 && p > 20000 && rng.chance(0.01)) p += 50;
    return static_cast<Price>(std::clamp<std::int64_t>(p, 1, 4'000'000'000));
  };

  int mismatches = 0;
  for (int i = 0; i < 400000 && mismatches == 0; ++i) {
    std::uint16_t locate = 0;
    const double r = rng.unit();
    if (r < 0.45 || live.empty()) {
      locate = static_cast<std::uint16_t>(1 + rng.below(8));
      const Side side = rng.chance(0.5) ? Side::Buy : Side::Sell;
      const Price price = quote(locate, side);
      const std::uint32_t shares =
          rng.chance(0.001) ? 0 : static_cast<std::uint32_t>(1 + rng.below(1000));
      const bool duplicate = !live.empty() && rng.chance(0.001);
      const std::uint64_t id = duplicate ? live[rng.below(live.size())].ref : next_ref++;
      fast.add(locate, id, side, shares, price);
      ref.add(locate, id, side, shares, price);
      if (shares != 0 && !duplicate) live.push_back({id, locate, shares, side});
    } else {
      const std::size_t k = rng.below(live.size());
      Live& o = live[k];
      locate = o.locate;
      const double q = rng.unit();
      if (q < 0.35) {
        fast.remove(o.ref);
        ref.remove(o.ref);
        live[k] = live.back();
        live.pop_back();
      } else if (q < 0.6) {
        std::uint32_t n = static_cast<std::uint32_t>(1 + rng.below(o.shares));
        if (rng.chance(0.01)) n = o.shares + 5;
        fast.execute(o.ref, n);
        ref.execute(o.ref, n);
        if (n >= o.shares) {
          live[k] = live.back();
          live.pop_back();
        } else {
          o.shares -= n;
        }
      } else if (q < 0.75) {
        if (o.shares > 1) {
          const auto n = static_cast<std::uint32_t>(1 + rng.below(o.shares - 1));
          fast.cancel(o.ref, n);
          ref.cancel(o.ref, n);
          o.shares -= n;
        }
      } else if (q < 0.97) {
        const std::uint64_t id = next_ref++;
        const Price price = quote(locate, o.side);
        const auto shares = static_cast<std::uint32_t>(1 + rng.below(1000));
        fast.replace(o.ref, id, shares, price);
        ref.replace(o.ref, id, shares, price);
        o.ref = id;
        o.shares = shares;
      } else {
        const std::int64_t ticks = rng.chance(0.2)
                                       ? 2000 + static_cast<std::int64_t>(rng.below(6000))
                                       : 1 + static_cast<std::int64_t>(rng.below(20));
        const std::int64_t lo = tick[locate] == 1 ? 100 : 20000;
        mid[locate] = std::clamp<std::int64_t>(
            mid[locate] + (rng.chance(0.5) ? ticks : -ticks) * tick[locate], lo, 40'000'000);
        fast.execute(next_ref + 1'000'000, 5);
        ref.execute(next_ref + 1'000'000, 5);
      }
    }
    if (fast.top(locate) != ref.top(locate)) {
      ++mismatches;
      std::printf("  first mismatch at step %d, locate %u\n", i, locate);
    }
  }
  CHECK(mismatches == 0);
  CHECK(fast.anomalies() == ref.anomalies());
  CHECK(fast.live_orders() == ref.live_orders());
  for (std::uint16_t l = 0; l < 16; ++l) CHECK(fast.top(l) == ref.top(l));
  CHECK(fast.stats().recenters > 0);
  CHECK(fast.stats().far_updates > 0);
}

TEST(fast_book_does_not_allocate_after_construction) {
  book::FastBook b(small_config());
  const std::uint64_t before = util::heap_allocations();
  for (std::uint64_t i = 1; i <= 50000; ++i) {
    const auto locate = static_cast<std::uint16_t>(1 + i % 8);
    const auto price = static_cast<Price>(100000 + (i % 7000) * 100);
    b.add(locate, i, i % 2 == 0 ? Side::Buy : Side::Sell, 100, price);
    if (i % 3 == 0) b.remove(i - 1);
    if (i % 5 == 0) b.replace(i, i + 1'000'000, 50, price + 100);
    static_cast<void>(b.top(locate));
  }
  CHECK(util::heap_allocations() == before);
  CHECK(b.stats().far_updates > 0);
}

TEST(order_map_matches_unordered_map) {
  book::OrderMap map(64);
  std::unordered_map<std::uint64_t, std::uint32_t> expected;
  Rng rng(9);
  int disagreements = 0;
  for (std::uint32_t i = 0; i < 200000; ++i) {
    const std::uint64_t key = rng.below(96);
    const std::size_t entry = map.find(key);
    const auto it = expected.find(key);
    if ((entry != book::OrderMap::kNotFound) != (it != expected.end())) {
      ++disagreements;
      continue;
    }
    if (it != expected.end()) {
      if (map.value(entry) != it->second) ++disagreements;
      if (rng.chance(0.5)) {
        map.erase(entry);
        expected.erase(it);
      }
    } else if (expected.size() < 32) {
      if (!map.insert(key, i)) ++disagreements;
      expected.emplace(key, i);
    }
  }
  CHECK(disagreements == 0);
  CHECK(map.size() == expected.size());
}

TEST(histogram_quantiles) {
  util::Histogram h;
  for (std::uint64_t v = 1; v <= 1000; ++v) h.record(v);
  CHECK(h.quantile(0.5) == 500);
  CHECK(h.quantile(0.99) == 990);
  CHECK(h.max() == 1000);
  util::Histogram wide;
  wide.record(1'000'000);
  wide.record(2'000'000);
  const std::uint64_t median = wide.quantile(0.5);
  CHECK(median >= 1'000'000 && median <= 1'016'000);
}

}  // namespace

int main() {
  for (const Test& t : registry()) {
    const int before = g_failures;
    t.fn();
    std::printf("%s %s\n", g_failures == before ? "pass" : "FAIL", t.name);
  }
  std::printf("%d failed checks\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
