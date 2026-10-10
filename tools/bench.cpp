#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
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
#include "util/cpu.hpp"
#include "util/histogram.hpp"

namespace {

constexpr const char* kUsage =
    "usage: itch_bench FILE [options]\n"
    "  --book fast|reference|decode  what to replay into (default fast)\n"
    "  --cpu N                       pin the replay thread to logical CPU N\n"
    "  --max-orders N                fast book order capacity\n"
    "  --no-latency                  skip the per-message latency pass\n";

using Clock = std::chrono::steady_clock;

enum class Mode { Fast, Reference, Decode };

// Reads every field a book would, so decode-only timing measures real work.
struct DecodeOnly {
  std::uint64_t checksum = 0;
  void on(const itch::AddOrder& m) { checksum += m.ref() + m.shares() + m.price() + m.locate(); }
  void on(const itch::AddOrderMpid& m) {
    checksum += m.ref() + m.shares() + m.price() + m.locate();
  }
  void on(const itch::OrderExecuted& m) { checksum += m.ref() + m.shares(); }
  void on(const itch::OrderExecutedWithPrice& m) { checksum += m.ref() + m.shares(); }
  void on(const itch::OrderCancel& m) { checksum += m.ref() + m.shares(); }
  void on(const itch::OrderDelete& m) { checksum += m.ref(); }
  void on(const itch::OrderReplace& m) {
    checksum += m.old_ref() + m.new_ref() + m.shares() + m.price();
  }
  void on(const itch::Message& m) { checksum += m.locate(); }
};

struct LatencyByType {
  util::Histogram all;
  std::vector<util::Histogram> by_type = std::vector<util::Histogram>(128);

  void record(char type, std::uint64_t ticks) {
    all.record(ticks);
    by_type[static_cast<unsigned char>(type) & 127].record(ticks);
  }
};

struct Pass {
  double seconds;
  std::uint64_t allocations;
};

template <class Handler>
Pass throughput(const itch::MappedFile& file, const itch::FileSummary& s, Handler& h) {
  const std::uint64_t before = util::heap_allocations();
  const auto t0 = Clock::now();
  itch::parse(file.data(), s.bytes, h);
  const auto t1 = Clock::now();
  return {std::chrono::duration<double>(t1 - t0).count(), util::heap_allocations() - before};
}

template <class Handler>
void latency(const itch::MappedFile& file, const itch::FileSummary& s, Handler& h,
             LatencyByType& out) {
  itch::for_each_message(file.data(), s.bytes, [&](const char* msg, std::size_t len) {
    const std::uint64_t t0 = util::tsc_begin();
    itch::dispatch(msg, len, h);
    const std::uint64_t t1 = util::tsc_end();
    out.record(msg[0], t1 - t0);
  });
}

double timer_overhead_ns(double ticks_per_ns) {
  util::Histogram h;
  for (int i = 0; i < 1'000'000; ++i) {
    const std::uint64_t t0 = util::tsc_begin();
    const std::uint64_t t1 = util::tsc_end();
    h.record(t1 - t0);
  }
  return static_cast<double>(h.quantile(0.5)) / ticks_per_ns;
}

void print_throughput(const itch::FileSummary& s, const Pass& p) {
  const auto n = static_cast<double>(s.messages);
  std::printf("throughput   %.2f M msg/s, %.1f ns/msg, %.2f s, %s heap allocations\n",
              n / p.seconds / 1e6, p.seconds * 1e9 / n, p.seconds,
              tools::commas(p.allocations).c_str());
}

void print_latency(const LatencyByType& lat, double ticks_per_ns) {
  const auto ns = [&](std::uint64_t ticks) { return static_cast<double>(ticks) / ticks_per_ns; };
  const auto row = [&](const std::string& name, const util::Histogram& h) {
    std::printf("%-20s %13s %7.0f %7.0f %7.0f %7.0f %8.0f %10.0f\n", name.c_str(),
                tools::commas(h.count()).c_str(), ns(h.quantile(0.5)), ns(h.quantile(0.9)),
                ns(h.quantile(0.99)), ns(h.quantile(0.999)), ns(h.quantile(0.9999)), ns(h.max()));
  };
  std::printf("\nlatency per message in ns (decode and apply, timer overhead included)\n");
  std::printf("%-20s %13s %7s %7s %7s %7s %8s %10s\n", "type", "count", "p50", "p90", "p99",
              "p99.9", "p99.99", "max");
  row("all", lat.all);
  std::vector<int> types;
  for (int t = 0; t < 128; ++t) {
    if (lat.by_type[static_cast<std::size_t>(t)].count() != 0) types.push_back(t);
  }
  std::sort(types.begin(), types.end(), [&](int a, int b) {
    return lat.by_type[static_cast<std::size_t>(a)].count() >
           lat.by_type[static_cast<std::size_t>(b)].count();
  });
  for (const int t : types) {
    const char type = static_cast<char>(t);
    row(std::string(1, type) + " " + itch::message_name(type),
        lat.by_type[static_cast<std::size_t>(t)]);
  }
}

template <class Book>
void print_book(const Book& b) {
  std::printf("orders       peak %s live, %s at end, %s feed anomalies\n",
              tools::commas(b.peak_orders()).c_str(), tools::commas(b.live_orders()).c_str(),
              tools::commas(b.anomalies().total()).c_str());
  if constexpr (requires { b.stats(); }) {
    std::printf("memory       %s preallocated, large pages: %s\n",
                tools::bytes_text(b.preallocated_bytes()).c_str(), b.large_pages() ? "yes" : "no");
    std::printf("slow path    %s window recenters, %s far-level updates\n",
                tools::commas(b.stats().recenters).c_str(),
                tools::commas(b.stats().far_updates).c_str());
  }
}

template <class Book, class Make>
void run_book(Make make, const itch::MappedFile& file, const itch::FileSummary& s,
              bool with_latency, double ticks_per_ns) {
  {
    const std::unique_ptr<Book> b = make();
    book::FeedHandler<Book> feed(*b);
    print_throughput(s, throughput(file, s, feed));
    print_book(*b);
  }
  if (with_latency) {
    const std::unique_ptr<Book> b = make();
    book::FeedHandler<Book> feed(*b);
    LatencyByType lat;
    latency(file, s, feed, lat);
    print_latency(lat, ticks_per_ns);
  }
}

int run(int argc, char** argv) {
  std::string path;
  Mode mode = Mode::Fast;
  int cpu = -1;
  std::uint64_t max_orders = 0;
  bool with_latency = true;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
    if (arg == "--book") {
      const std::string_view v = value == nullptr ? "" : value;
      if (v == "fast") {
        mode = Mode::Fast;
      } else if (v == "reference") {
        mode = Mode::Reference;
      } else if (v == "decode") {
        mode = Mode::Decode;
      } else {
        throw std::invalid_argument("--book takes fast, reference or decode");
      }
      ++i;
    } else if (arg == "--cpu") {
      cpu = static_cast<int>(tools::parse_count(arg, value));
      ++i;
    } else if (arg == "--max-orders") {
      max_orders = tools::parse_count(arg, value);
      ++i;
    } else if (arg == "--no-latency") {
      with_latency = false;
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
  if (s.messages == 0) return 0;
  if (cpu >= 0 && !util::pin_thread(cpu))
    std::printf("warning      could not pin to cpu %d\n", cpu);

  const double ticks_per_ns = util::tsc_ticks_per_ns();
  std::printf("clock        %.3f GHz timestamp counter, timer overhead %.1f ns\n", ticks_per_ns,
              timer_overhead_ns(ticks_per_ns));

  switch (mode) {
    case Mode::Fast: {
      std::printf("book         fast\n");
      const book::FastBookConfig config = tools::fast_config(s, max_orders);
      run_book<book::FastBook>([&] { return std::make_unique<book::FastBook>(config); }, file, s,
                               with_latency, ticks_per_ns);
      break;
    }
    case Mode::Reference:
      std::printf("book         reference\n");
      run_book<book::ReferenceBook>([] { return std::make_unique<book::ReferenceBook>(); }, file, s,
                                    with_latency, ticks_per_ns);
      break;
    case Mode::Decode: {
      std::printf("book         none (decode only)\n");
      DecodeOnly decode;
      print_throughput(s, throughput(file, s, decode));
      if (with_latency) {
        LatencyByType lat;
        latency(file, s, decode, lat);
        print_latency(lat, ticks_per_ns);
      }
      std::printf("checksum     %llu\n", static_cast<unsigned long long>(decode.checksum));
      break;
    }
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
