#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "book/fast_book.hpp"
#include "book/feed.hpp"
#include "book/reference_book.hpp"
#include "common.hpp"
#include "itch/mapped_file.hpp"
#include "itch/parser.hpp"
#include "itch/summary.hpp"
#include "util/alloc_counter.hpp"

namespace {

constexpr const char* kUsage =
    "usage: itch_verify FILE [options]\n"
    "  --max-orders N  fast book order capacity\n"
    "  --show N        details to print for the first N problems (default 10)\n";

// Replays the feed into both books and compares top of book after every order event.
class Checker {
 public:
  Checker(const itch::FileSummary& summary, book::FastBook& fast, book::ReferenceBook& ref,
          std::uint64_t show)
      : summary_(summary),
        fast_(fast),
        ref_(ref),
        fast_feed_(fast),
        ref_feed_(ref),
        show_(show),
        trading_(std::size_t{summary.max_locate} + 1, true) {}

  void on(const itch::AddOrder& m) { book_event(m); }
  void on(const itch::AddOrderMpid& m) { book_event(m); }
  void on(const itch::OrderExecuted& m) { book_event(m); }
  void on(const itch::OrderExecutedWithPrice& m) { book_event(m); }
  void on(const itch::OrderCancel& m) { book_event(m); }
  void on(const itch::OrderDelete& m) { book_event(m); }
  void on(const itch::OrderReplace& m) { book_event(m); }

  void on(const itch::SystemEvent& m) {
    if (m.event_code() == 'Q') market_hours_ = true;
    if (m.event_code() == 'M') market_hours_ = false;
  }

  void on(const itch::StockTradingAction& m) {
    if (m.locate() < trading_.size()) trading_[m.locate()] = m.trading_state() == 'T';
  }

  void next() noexcept { ++index_; }

  std::uint64_t events() const noexcept { return events_; }
  std::uint64_t mismatches() const noexcept { return mismatches_; }
  std::uint64_t crossed() const noexcept { return crossed_; }
  std::uint64_t fast_allocations() const noexcept { return fast_allocations_; }

 private:
  template <class M>
  void book_event(const M& m) {
    const std::uint64_t before = util::heap_allocations();
    fast_feed_.on(m);
    fast_allocations_ += util::heap_allocations() - before;
    ref_feed_.on(m);
    ++events_;

    const std::uint16_t locate = m.locate();
    const book::TopOfBook expected = ref_.top(locate);
    const book::TopOfBook actual = fast_.top(locate);
    if (actual != expected && mismatches_++ < show_) {
      describe("mismatch", m, locate);
      std::printf("  reference  bid %s, ask %s\n", tools::quote_text(expected.bid).c_str(),
                  tools::quote_text(expected.ask).c_str());
      std::printf("  fast       bid %s, ask %s\n", tools::quote_text(actual.bid).c_str(),
                  tools::quote_text(actual.ask).c_str());
    }

    const bool two_sided = expected.bid.qty != 0 && expected.ask.qty != 0;
    if (market_hours_ && two_sided && expected.bid.price >= expected.ask.price &&
        locate < trading_.size() && trading_[locate] && crossed_++ < show_) {
      describe("crossed", m, locate);
      std::printf("  bid %s, ask %s\n", tools::quote_text(expected.bid).c_str(),
                  tools::quote_text(expected.ask).c_str());
    }
  }

  void describe(const char* what, const itch::Message& m, std::uint16_t locate) const {
    std::printf("%s at message %s (%c %s) %s %s\n", what, tools::commas(index_ + 1).c_str(),
                m.type(), itch::message_name(m.type()),
                tools::symbol_name(summary_, locate).c_str(),
                tools::clock_text(m.timestamp()).c_str());
  }

  const itch::FileSummary& summary_;
  book::FastBook& fast_;
  book::ReferenceBook& ref_;
  book::FeedHandler<book::FastBook> fast_feed_;
  book::FeedHandler<book::ReferenceBook> ref_feed_;
  std::uint64_t show_;
  std::vector<bool> trading_;
  bool market_hours_ = false;
  std::uint64_t index_ = 0;
  std::uint64_t events_ = 0;
  std::uint64_t mismatches_ = 0;
  std::uint64_t crossed_ = 0;
  std::uint64_t fast_allocations_ = 0;
};

std::string anomaly_text(const book::Anomalies& a) {
  return tools::commas(a.total()) + " (unknown " + tools::commas(a.unknown_order) + ", duplicate " +
         tools::commas(a.duplicate_order) + ", zero shares " + tools::commas(a.zero_shares) +
         ", overfill " + tools::commas(a.overfill) + ", bad locate " + tools::commas(a.bad_locate) +
         ")";
}

int run(int argc, char** argv) {
  std::string path;
  std::uint64_t max_orders = 0;
  std::uint64_t show = 10;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
    if (arg == "--max-orders") {
      max_orders = tools::parse_count(arg, value);
      ++i;
    } else if (arg == "--show") {
      show = tools::parse_count(arg, value);
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
  const itch::FileSummary s = itch::summarize(file.data(), file.size());
  tools::print_file(path, file.size(), s);

  book::FastBook fast(tools::fast_config(s, max_orders));
  book::ReferenceBook ref;
  Checker checker(s, fast, ref, show);

  const std::uint64_t step = s.messages >= 10'000'000 ? s.messages / 10 : 0;
  std::uint64_t done = 0;
  const auto t0 = std::chrono::steady_clock::now();
  itch::for_each_message(file.data(), s.bytes, [&](const char* msg, std::size_t len) {
    itch::dispatch(msg, len, checker);
    checker.next();
    if (step != 0 && ++done % step == 0) {
      std::fprintf(stderr, "  %3llu%%\n", static_cast<unsigned long long>(done / step * 10));
    }
  });
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  const bool anomalies_match = fast.anomalies() == ref.anomalies();
  const bool live_match = fast.live_orders() == ref.live_orders();
  const bool pass = checker.mismatches() == 0 && anomalies_match && live_match &&
                    checker.fast_allocations() == 0 && s.malformed == 0;

  std::printf("replay       %s messages in %.2f s\n", tools::commas(s.messages).c_str(), seconds);
  std::printf("compared     top of book after %s order events: %s mismatches\n",
              tools::commas(checker.events()).c_str(), tools::commas(checker.mismatches()).c_str());
  std::printf("anomalies    fast %s\n", anomaly_text(fast.anomalies()).c_str());
  std::printf("             reference %s\n", anomaly_text(ref.anomalies()).c_str());
  std::printf("live orders  fast %s, reference %s at end of feed (peak %s)\n",
              tools::commas(fast.live_orders()).c_str(), tools::commas(ref.live_orders()).c_str(),
              tools::commas(ref.peak_orders()).c_str());
  std::printf(
      "crossed      %s order events left a trading symbol locked or crossed in market hours\n",
      tools::commas(checker.crossed()).c_str());
  std::printf(
      "allocations  %s heap allocations inside the fast book (%s preallocated, "
      "large pages: %s)\n",
      tools::commas(checker.fast_allocations()).c_str(),
      tools::bytes_text(fast.preallocated_bytes()).c_str(), fast.large_pages() ? "yes" : "no");
  std::printf("slow path    %s window recenters, %s far-level updates\n",
              tools::commas(fast.stats().recenters).c_str(),
              tools::commas(fast.stats().far_updates).c_str());
  std::printf("result       %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
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
