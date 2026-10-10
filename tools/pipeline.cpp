#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "book/fast_book.hpp"
#include "book/feed.hpp"
#include "common.hpp"
#include "itch/mapped_file.hpp"
#include "itch/parser.hpp"
#include "itch/summary.hpp"
#include "util/cpu.hpp"
#include "util/histogram.hpp"
#include "util/spsc_ring.hpp"

namespace {

constexpr const char* kUsage =
    "usage: itch_pipeline FILE [options]\n"
    "  --feed-cpu N      pin the feed thread to logical CPU N\n"
    "  --strategy-cpu N  pin the strategy thread to logical CPU N\n"
    "  --out PATH        write per-minute order flow imbalance as CSV\n"
    "  --max-orders N    fast book order capacity\n";

constexpr std::uint64_t kNanosPerMinute = 60'000'000'000;

enum class Kind : std::uint8_t { Quote, MarketOpen, MarketClose, Halt, Resume, End };

struct Update {
  std::uint64_t sent = 0;
  std::uint64_t timestamp = 0;
  book::Qty bid_qty = 0;
  book::Qty ask_qty = 0;
  book::Price bid_price = 0;
  book::Price ask_price = 0;
  std::uint16_t locate = 0;
  Kind kind = Kind::Quote;
};

using Ring = util::SpscRing<Update>;

// Feed thread: applies each message to the book and publishes the top of book whenever it
// changes, along with the session and halt events the strategy needs.
class Publisher {
 public:
  Publisher(book::FastBook& book, Ring& ring, std::size_t num_locates)
      : book_(book), feed_(book), ring_(ring), last_(num_locates) {}

  void on(const itch::AddOrder& m) { book_event(m); }
  void on(const itch::AddOrderMpid& m) { book_event(m); }
  void on(const itch::OrderExecuted& m) { book_event(m); }
  void on(const itch::OrderExecutedWithPrice& m) { book_event(m); }
  void on(const itch::OrderCancel& m) { book_event(m); }
  void on(const itch::OrderDelete& m) { book_event(m); }
  void on(const itch::OrderReplace& m) { book_event(m); }

  void on(const itch::SystemEvent& m) {
    if (m.event_code() == 'Q') publish(control(Kind::MarketOpen, m));
    if (m.event_code() == 'M') publish(control(Kind::MarketClose, m));
  }

  void on(const itch::StockTradingAction& m) {
    publish(control(m.trading_state() == 'T' ? Kind::Resume : Kind::Halt, m));
  }

  void finish() {
    Update u;
    u.kind = Kind::End;
    publish(u);
  }

  std::uint64_t published() const noexcept { return published_; }
  std::uint64_t waits() const noexcept { return waits_; }

 private:
  template <class M>
  void book_event(const M& m) {
    feed_.on(m);
    const std::uint16_t locate = m.locate();
    if (locate >= last_.size()) return;
    const book::TopOfBook top = book_.top(locate);
    if (top == last_[locate]) return;
    last_[locate] = top;
    Update u;
    u.timestamp = m.timestamp();
    u.locate = locate;
    u.bid_price = top.bid.price;
    u.bid_qty = top.bid.qty;
    u.ask_price = top.ask.price;
    u.ask_qty = top.ask.qty;
    publish(u);
  }

  static Update control(Kind kind, const itch::Message& m) {
    Update u;
    u.kind = kind;
    u.locate = m.locate();
    u.timestamp = m.timestamp();
    return u;
  }

  void publish(Update u) {
    u.sent = util::tsc_now();
    if (!ring_.try_push(u)) {
      ++waits_;
      do {
        util::cpu_relax();
      } while (!ring_.try_push(u));
    }
    ++published_;
  }

  book::FastBook& book_;
  book::FeedHandler<book::FastBook> feed_;
  Ring& ring_;
  std::vector<book::TopOfBook> last_;
  std::uint64_t published_ = 0;
  std::uint64_t waits_ = 0;
};

struct OfiRow {
  std::uint16_t locate;
  std::uint32_t minute;
  std::int64_t ofi;
  std::uint32_t events;
  double mid_open;
  double mid_close;
};

// Order flow imbalance from Cont, Kukanov and Stoikov (2014), "The Price Impact of Order Book
// Events": each top of book change contributes the bid size added minus removed, less the same
// for the ask. Summed per symbol per minute during regular hours, skipping halted symbols.
class OfiEngine {
 public:
  explicit OfiEngine(std::size_t num_locates) : symbols_(num_locates) {
    rows_.reserve(std::size_t{1} << 20);
  }

  void on(const Update& u) {
    switch (u.kind) {
      case Kind::Quote:
        quote(u);
        break;
      case Kind::MarketOpen:
        open_ = true;
        break;
      case Kind::MarketClose:
        flush_all();
        open_ = false;
        break;
      case Kind::Halt:
        if (u.locate < symbols_.size()) {
          flush(u.locate);
          symbols_[u.locate].halted = true;
        }
        break;
      case Kind::Resume:
        if (u.locate < symbols_.size()) symbols_[u.locate].halted = false;
        break;
      case Kind::End:
        break;
    }
  }

  void finish() { flush_all(); }
  std::vector<OfiRow>& rows() noexcept { return rows_; }

 private:
  static constexpr std::uint32_t kNoMinute = ~std::uint32_t{0};

  struct State {
    book::Price bid_price = 0;
    book::Price ask_price = 0;
    book::Qty bid_qty = 0;
    book::Qty ask_qty = 0;
    bool halted = false;
    std::uint32_t minute = kNoMinute;
    std::int64_t ofi = 0;
    std::uint32_t events = 0;
    double mid_open = 0;
    double mid_last = 0;
  };

  static std::int64_t imbalance(const State& prev, const Update& u) {
    std::int64_t e = 0;
    if (u.bid_price >= prev.bid_price) e += static_cast<std::int64_t>(u.bid_qty);
    if (u.bid_price <= prev.bid_price) e -= static_cast<std::int64_t>(prev.bid_qty);
    if (u.ask_price <= prev.ask_price) e -= static_cast<std::int64_t>(u.ask_qty);
    if (u.ask_price >= prev.ask_price) e += static_cast<std::int64_t>(prev.ask_qty);
    return e;
  }

  void quote(const Update& u) {
    State& s = symbols_[u.locate];
    const bool two_sided = u.bid_qty != 0 && u.ask_qty != 0;
    const bool was_two_sided = s.bid_qty != 0 && s.ask_qty != 0;
    if (open_ && !s.halted && two_sided) {
      const double mid =
          (static_cast<double>(u.bid_price) + static_cast<double>(u.ask_price)) / 20000.0;
      const auto minute = static_cast<std::uint32_t>(u.timestamp / kNanosPerMinute);
      if (minute != s.minute) {
        flush(u.locate);
        s.minute = minute;
        s.mid_open = s.mid_last != 0 ? s.mid_last : mid;
      }
      if (was_two_sided) {
        s.ofi += imbalance(s, u);
        ++s.events;
      }
      s.mid_last = mid;
    }
    s.bid_price = u.bid_price;
    s.bid_qty = u.bid_qty;
    s.ask_price = u.ask_price;
    s.ask_qty = u.ask_qty;
  }

  void flush(std::uint16_t locate) {
    State& s = symbols_[locate];
    if (s.minute != kNoMinute && s.events != 0) {
      rows_.push_back({locate, s.minute, s.ofi, s.events, s.mid_open, s.mid_last});
    }
    s.minute = kNoMinute;
    s.ofi = 0;
    s.events = 0;
  }

  void flush_all() {
    for (std::size_t i = 0; i < symbols_.size(); ++i) flush(static_cast<std::uint16_t>(i));
  }

  std::vector<State> symbols_;
  std::vector<OfiRow> rows_;
  bool open_ = false;
};

void run_strategy(Ring& ring, OfiEngine& engine, util::Histogram& handoff, std::uint64_t& skewed) {
  Update u;
  for (;;) {
    if (!ring.try_pop(u)) {
      util::cpu_relax();
      continue;
    }
    const std::uint64_t now = util::tsc_now();
    if (now < u.sent) ++skewed;
    handoff.record(util::elapsed_ticks(u.sent, now));
    if (u.kind == Kind::End) break;
    engine.on(u);
  }
  engine.finish();
}

void write_csv(const std::string& path, std::vector<OfiRow>& rows, const itch::FileSummary& s) {
  std::sort(rows.begin(), rows.end(), [](const OfiRow& a, const OfiRow& b) {
    return a.locate != b.locate ? a.locate < b.locate : a.minute < b.minute;
  });
  const tools::File f = tools::open_file(path, "w");
  std::fprintf(f.get(), "symbol,minute,ofi,events,mid_open,mid_close\n");
  for (const OfiRow& r : rows) {
    std::fprintf(f.get(), "%s,%02u:%02u,%lld,%u,%.4f,%.4f\n",
                 tools::symbol_name(s, r.locate).c_str(), r.minute / 60, r.minute % 60,
                 static_cast<long long>(r.ofi), r.events, r.mid_open, r.mid_close);
  }
}

int run(int argc, char** argv) {
  std::string path;
  std::string out;
  int feed_cpu = -1;
  int strategy_cpu = -1;
  std::uint64_t max_orders = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
    if (arg == "--feed-cpu") {
      feed_cpu = static_cast<int>(tools::parse_count(arg, value));
      ++i;
    } else if (arg == "--strategy-cpu") {
      strategy_cpu = static_cast<int>(tools::parse_count(arg, value));
      ++i;
    } else if (arg == "--out") {
      if (value == nullptr) throw std::invalid_argument("--out needs a path");
      out = value;
      ++i;
    } else if (arg == "--max-orders") {
      max_orders = tools::parse_count(arg, value);
      ++i;
    } else if (arg == "-h" || arg == "--help") {
      std::fputs(kUsage, stdout);
      return 0;
    } else if (!arg.empty() && arg[0] == '-') {
      throw std::invalid_argument("unknown option " + std::string(arg));
    } else {
      path = arg;
    }
  }
  if (path.empty()) {
    std::fputs(kUsage, stderr);
    return 2;
  }

  const itch::MappedFile file(path);
  file.prefault();
  const itch::FileSummary s = itch::summarize(file.data(), file.size());
  tools::print_file(path, file.size(), s);

  const std::size_t num_locates = std::size_t{s.max_locate} + 1;
  book::FastBook book(tools::fast_config(s, max_orders));
  Ring ring(std::size_t{1} << 16);
  OfiEngine engine(num_locates);
  util::Histogram handoff;
  std::uint64_t skewed = 0;
  Publisher publisher(book, ring, num_locates);
  const double ticks_per_ns = util::tsc_ticks_per_ns();

  bool strategy_pinned = true;
  const auto t0 = std::chrono::steady_clock::now();
  std::thread strategy([&] {
    if (strategy_cpu >= 0) strategy_pinned = util::pin_thread(strategy_cpu);
    run_strategy(ring, engine, handoff, skewed);
  });
  if (feed_cpu >= 0 && !util::pin_thread(feed_cpu)) {
    std::printf("warning      could not pin the feed thread to cpu %d\n", feed_cpu);
  }
  itch::parse(file.data(), s.bytes, publisher);
  publisher.finish();
  strategy.join();
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!strategy_pinned) {
    std::printf("warning      could not pin the strategy thread to cpu %d\n", strategy_cpu);
  }

  const auto ns = [&](double q) { return static_cast<double>(handoff.quantile(q)) / ticks_per_ns; };
  std::printf("feed         %s messages in %.2f s (%.2f M msg/s), %s top of book updates\n",
              tools::commas(s.messages).c_str(), seconds,
              static_cast<double>(s.messages) / seconds / 1e6,
              tools::commas(publisher.published()).c_str());
  std::printf("ring         %s slots, feed waited for space %s times\n",
              tools::commas(ring.capacity()).c_str(), tools::commas(publisher.waits()).c_str());
  std::printf("handoff      p50 %.0f ns, p99 %.0f ns, p99.9 %.0f ns, max %.0f ns\n", ns(0.5),
              ns(0.99), ns(0.999), static_cast<double>(handoff.max()) / ticks_per_ns);
  std::printf(
      "clock skew   %s handoff samples read the strategy core's counter behind the feed core's\n",
      tools::commas(skewed).c_str());
  std::printf("ofi          %s symbol-minutes in regular hours\n",
              tools::commas(engine.rows().size()).c_str());
  if (!out.empty()) {
    write_csv(out, engine.rows(), s);
    std::printf("wrote        %s\n", out.c_str());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
