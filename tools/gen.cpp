#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"
#include "itch/writer.hpp"

namespace {

constexpr const char* kUsage =
    "usage: itch_gen OUTPUT [options]\n"
    "  --messages N  approximate number of messages (default 5000000)\n"
    "  --symbols N   number of symbols (default 64)\n"
    "  --seed N      random seed (default 1)\n";

constexpr std::uint64_t kHour = 3'600'000'000'000;
constexpr std::uint64_t kMinute = 60'000'000'000;
constexpr std::string_view kMpids[] = {"NSDQ", "GSCO", "MSCO", "UBSS"};

// splitmix64, so a seed produces the same file on every platform.
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

struct Order {
  std::uint64_t ref;
  std::uint32_t price;
  std::uint32_t shares;
  char side;
};

struct Symbol {
  std::string name;
  std::uint16_t locate = 0;
  std::int64_t mid = 0;  // dollars times 10,000, on the symbol's tick grid
  std::int64_t tick = 100;
  bool halted = false;
  std::vector<Order> orders;
};

struct Scheduled {
  std::uint64_t ts;
  char kind;  // a system event code, or 'h' / 'r' to halt or resume the second symbol
};

// Synthetic feed with the shape of a real day: sub-dollar and high-priced symbols, deep and
// off-grid orders, price jumps that cross resting orders, a trading halt and every message type.
class Generator {
 public:
  Generator(std::FILE* out, std::uint64_t target, std::size_t symbols, std::uint64_t seed)
      : out_(out), target_(target), rng_(seed) {
    for (std::size_t i = 0; i < symbols; ++i) {
      Symbol s;
      s.locate = static_cast<std::uint16_t>(i + 1);
      s.name = "T" + letters(i);
      if (i % 8 == 3) {
        s.tick = 1;
        s.mid = 500 + static_cast<std::int64_t>(rng_.below(8500));
      } else if (i % 8 == 5) {
        s.mid = (500 + static_cast<std::int64_t>(rng_.below(2500))) * 10000;
      } else {
        s.mid = (5 + static_cast<std::int64_t>(rng_.below(195))) * 10000;
      }
      symbols_.push_back(std::move(s));
    }
    step_ = std::max<std::uint64_t>(1, 16 * kHour / std::max<std::uint64_t>(target_, 1));
    schedule_ = {{9 * kHour + 30 * kMinute, 'Q'},
                 {11 * kHour, 'h'},
                 {11 * kHour + 5 * kMinute, 'r'},
                 {16 * kHour, 'M'},
                 {20 * kHour, 'E'}};
  }

  void run() {
    w_.system_event(3 * kHour, 'O');
    for (const Symbol& s : symbols_) {
      w_.stock_directory(s.locate, 3 * kHour + s.locate, s.name);
      w_.trading_action(s.locate, 3 * kHour + s.locate, s.name, 'T');
    }
    ts_ = 4 * kHour;
    w_.system_event(ts_, 'S');
    while (w_.messages() < target_) {
      advance();
      step();
      if (w_.bytes().size() >= (std::size_t{1} << 20)) flush();
    }
    for (; next_event_ < schedule_.size(); ++next_event_) fire(schedule_[next_event_]);
    w_.system_event(ts_ + kMinute, 'C');
    flush();
  }

  std::uint64_t messages() const noexcept { return w_.messages(); }
  std::uint64_t bytes() const noexcept { return bytes_; }
  std::size_t live_orders() const noexcept {
    std::size_t n = 0;
    for (const Symbol& s : symbols_) n += s.orders.size();
    return n;
  }

 private:
  static std::string letters(std::size_t i) {
    std::string s(3, 'A');
    for (std::size_t k = 3; k-- > 0; i /= 26) s[k] = static_cast<char>('A' + i % 26);
    return s;
  }

  void flush() {
    const std::vector<char>& b = w_.bytes();
    if (!b.empty() && std::fwrite(b.data(), 1, b.size(), out_) != b.size()) {
      throw std::runtime_error("write failed");
    }
    bytes_ += b.size();
    w_.clear();
  }

  void advance() {
    ts_ += step_ / 2 + rng_.below(step_ + 1);
    while (next_event_ < schedule_.size() && ts_ >= schedule_[next_event_].ts) {
      fire(schedule_[next_event_++]);
    }
  }

  void fire(const Scheduled& e) {
    const std::uint64_t ts = std::max(ts_, e.ts);
    if (e.kind == 'h' || e.kind == 'r') {
      if (symbols_.size() < 2) return;
      Symbol& s = symbols_[1];
      s.halted = e.kind == 'h';
      w_.trading_action(s.locate, ts, s.name, s.halted ? 'H' : 'T', s.halted ? "T1" : "");
      return;
    }
    w_.system_event(ts, e.kind);
  }

  Symbol& pick() {
    const double u = rng_.unit();
    auto i = static_cast<std::size_t>(u * u * static_cast<double>(symbols_.size()));
    i = std::min(i, symbols_.size() - 1);
    if (symbols_[i].halted) i = (i + 1) % symbols_.size();
    return symbols_[i];
  }

  void step() {
    Symbol& s = pick();
    if (s.halted) return;
    const double r = rng_.unit();
    if (s.orders.empty() || r < 0.42) {
      add(s);
    } else if (r < 0.74) {
      remove(s, pick_order(s));
    } else if (r < 0.83) {
      replace(s);
    } else if (r < 0.88) {
      cancel(s);
    } else if (r < 0.935) {
      execute(s, false);
    } else if (r < 0.94) {
      execute(s, true);
    } else if (r < 0.95) {
      w_.trade(s.locate, ts_, rng_.chance(0.5) ? 'B' : 'S', order_size(), s.name, mid(s), ++match_);
    } else if (r < 0.985) {
      move(s, false);
    } else if (r < 0.9852) {
      move(s, true);
    } else {
      other(s);
    }
  }

  std::size_t pick_order(const Symbol& s) { return rng_.below(s.orders.size()); }

  static std::uint32_t mid(const Symbol& s) { return static_cast<std::uint32_t>(s.mid); }

  std::uint32_t order_size() {
    const double r = rng_.unit();
    if (r < 0.75) return 100 * static_cast<std::uint32_t>(1 + rng_.below(10));
    if (r < 0.95) return static_cast<std::uint32_t>(1 + rng_.below(99));
    return 1000 * static_cast<std::uint32_t>(1 + rng_.below(50));
  }

  std::uint32_t quote_price(const Symbol& s, char side) {
    std::int64_t distance = 1;
    const double r = rng_.unit();
    if (r < 0.88) {
      while (distance < 30 && rng_.chance(0.55)) ++distance;
    } else if (r < 0.99) {
      distance = 5 + static_cast<std::int64_t>(rng_.below(400));
    } else {
      distance = 3000 + static_cast<std::int64_t>(rng_.below(12000));
    }
    std::int64_t p = side == 'B' ? s.mid - distance * s.tick : s.mid + distance * s.tick;
    if (s.tick == 100 && p > 20000 && rng_.chance(0.002)) p += side == 'B' ? -50 : 50;
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(p, 1, 4'000'000'000));
  }

  void add(Symbol& s) {
    const char side = rng_.chance(0.5) ? 'B' : 'S';
    const Order o{next_ref_++, quote_price(s, side), order_size(), side};
    if (rng_.chance(0.06)) {
      w_.add_order_mpid(s.locate, ts_, o.ref, side, o.shares, s.name, o.price,
                        kMpids[rng_.below(std::size(kMpids))]);
    } else {
      w_.add_order(s.locate, ts_, o.ref, side, o.shares, s.name, o.price);
    }
    s.orders.push_back(o);
  }

  void erase(Symbol& s, std::size_t i) {
    s.orders[i] = s.orders.back();
    s.orders.pop_back();
  }

  void remove(Symbol& s, std::size_t i) {
    w_.order_delete(s.locate, ts_, s.orders[i].ref);
    erase(s, i);
  }

  void cancel(Symbol& s) {
    const std::size_t i = pick_order(s);
    Order& o = s.orders[i];
    if (o.shares < 2) {
      remove(s, i);
      return;
    }
    const auto n = static_cast<std::uint32_t>(1 + rng_.below(o.shares - 1));
    w_.cancel(s.locate, ts_, o.ref, n);
    o.shares -= n;
  }

  void execute(Symbol& s, bool with_price) {
    const std::size_t i = pick_order(s);
    Order& o = s.orders[i];
    const std::uint32_t n =
        rng_.chance(0.5) ? o.shares : static_cast<std::uint32_t>(1 + rng_.below(o.shares));
    if (with_price) {
      const std::uint32_t price =
          o.price > s.tick ? o.price - static_cast<std::uint32_t>(s.tick) : o.price;
      w_.executed_with_price(s.locate, ts_, o.ref, n, ++match_, 'Y', price);
    } else {
      w_.executed(s.locate, ts_, o.ref, n, ++match_);
    }
    o.shares -= n;
    if (o.shares == 0) erase(s, i);
  }

  void replace(Symbol& s) {
    Order& o = s.orders[pick_order(s)];
    const Order next{next_ref_++, quote_price(s, o.side), order_size(), o.side};
    w_.replace(s.locate, ts_, o.ref, next.ref, next.shares, next.price);
    o = next;
  }

  // Moving the mid executes every resting order the new price runs through, so bids stay
  // below asks the way they do on the exchange.
  void move(Symbol& s, bool jump) {
    const std::int64_t ticks = jump ? 3000 + static_cast<std::int64_t>(rng_.below(3000))
                                    : 1 + static_cast<std::int64_t>(rng_.below(3));
    const std::int64_t delta = (rng_.chance(0.5) ? ticks : -ticks) * s.tick;
    const std::int64_t lo = s.tick == 1 ? 50 : 20000;
    const std::int64_t hi = s.tick == 1 ? 9900 : 3'000'000'000;
    s.mid = std::clamp(s.mid + delta, lo, hi);
    for (std::size_t i = 0; i < s.orders.size();) {
      const Order& o = s.orders[i];
      const auto price = static_cast<std::int64_t>(o.price);
      if (o.side == 'B' ? price >= s.mid : price <= s.mid) {
        w_.executed(s.locate, ts_, o.ref, o.shares, ++match_);
        erase(s, i);
      } else {
        ++i;
      }
    }
  }

  void other(Symbol& s) {
    const std::uint32_t m = mid(s);
    switch (rng_.below(12)) {
      case 0:
        w_.cross(s.locate, ts_, 10000 + rng_.below(1'000'000), s.name, m, ++match_, 'O');
        break;
      case 1:
        if (match_ != 0) w_.broken(s.locate, ts_, 1 + rng_.below(match_));
        break;
      case 2:
        w_.noii(s.locate, ts_, 50000, 1200, 'B', s.name, m, m, m, 'C', 'L');
        break;
      case 3:
        w_.reg_sho(s.locate, ts_, s.name, '1');
        break;
      case 4:
        w_.participant_position(s.locate, ts_, kMpids[0], s.name, 'Y', 'N', 'A');
        break;
      case 5:
        w_.retail_interest(s.locate, ts_, s.name, 'B');
        break;
      case 6:
        w_.ipo_quoting(s.locate, ts_, s.name, 34200, 'A', m);
        break;
      case 7:
        w_.luld_collar(s.locate, ts_, s.name, m, m + 500, m > 500 ? m - 500 : 1, 0);
        break;
      case 8:
        w_.operational_halt(s.locate, ts_, s.name, 'Q', 'T');
        break;
      case 9:
        w_.mwcb_decline(ts_, 3'000'000'000'000, 2'800'000'000'000, 2'600'000'000'000);
        break;
      case 10:
        w_.mwcb_status(ts_, '1');
        break;
      default:
        w_.direct_listing(s.locate, ts_, s.name, 'Y', m / 2, m * 2, m, ts_, m / 2, m * 2);
        break;
    }
  }

  std::FILE* out_;
  std::uint64_t target_;
  Rng rng_;
  itch::Writer w_;
  std::vector<Symbol> symbols_;
  std::vector<Scheduled> schedule_;
  std::size_t next_event_ = 0;
  std::uint64_t ts_ = 0;
  std::uint64_t step_ = 1;
  std::uint64_t next_ref_ = 1;
  std::uint64_t match_ = 0;
  std::uint64_t bytes_ = 0;
};

int run(int argc, char** argv) {
  std::string path;
  std::uint64_t messages = 5'000'000;
  std::uint64_t symbols = 64;
  std::uint64_t seed = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
    if (arg == "--messages") {
      messages = tools::parse_count(arg, value);
      ++i;
    } else if (arg == "--symbols") {
      symbols = tools::parse_count(arg, value);
      ++i;
    } else if (arg == "--seed") {
      seed = tools::parse_count(arg, value);
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
  if (symbols == 0 || symbols > 65535) throw std::invalid_argument("--symbols must be 1 to 65535");

  const tools::File out = tools::open_file(path, "wb");
  Generator g(out.get(), messages, static_cast<std::size_t>(symbols), seed);
  g.run();
  std::printf("wrote %s: %s messages, %s, %s symbols, %s orders left open\n", path.c_str(),
              tools::commas(g.messages()).c_str(), tools::bytes_text(g.bytes()).c_str(),
              tools::commas(symbols).c_str(), tools::commas(g.live_orders()).c_str());
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
