#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "book/fast_book.hpp"
#include "book/types.hpp"
#include "itch/messages.hpp"
#include "itch/summary.hpp"

namespace tools {

inline std::uint64_t parse_count(std::string_view flag, const char* value) {
  if (value == nullptr) throw std::invalid_argument(std::string(flag) + " needs a value");
  char* end = nullptr;
  const unsigned long long v = std::strtoull(value, &end, 10);
  if (end == value || *end != '\0') {
    throw std::invalid_argument("invalid value for " + std::string(flag) + ": " + value);
  }
  return v;
}

inline std::string commas(std::uint64_t v) {
  std::string s = std::to_string(v);
  for (auto i = static_cast<std::ptrdiff_t>(s.size()) - 3; i > 0; i -= 3) {
    s.insert(static_cast<std::size_t>(i), 1, ',');
  }
  return s;
}

inline std::string bytes_text(std::uint64_t bytes) {
  char buf[32];
  if (bytes >= 1'000'000'000) {
    std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(bytes) / 1e9);
  } else {
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / 1e6);
  }
  return buf;
}

inline std::string dollars(book::Price p) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%u.%04u", p / 10000, p % 10000);
  return buf;
}

inline std::string quote_text(const book::Quote& q) {
  if (q.qty == 0) return "none";
  return commas(q.qty) + " @ " + dollars(q.price) + " (" + std::to_string(q.orders) + " orders)";
}

inline std::string clock_text(std::uint64_t ns_since_midnight) {
  const std::uint64_t s = ns_since_midnight / 1'000'000'000;
  char buf[48];
  std::snprintf(
      buf, sizeof buf, "%02llu:%02llu:%02llu.%09llu", static_cast<unsigned long long>(s / 3600),
      static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60),
      static_cast<unsigned long long>(ns_since_midnight % 1'000'000'000));
  return buf;
}

struct CloseFile {
  void operator()(std::FILE* f) const noexcept { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, CloseFile>;

inline File open_file(const std::string& path, const char* mode) {
  File f(std::fopen(path.c_str(), mode));
  if (!f) throw std::runtime_error("cannot open " + path);
  return f;
}

inline std::string symbol_name(const itch::FileSummary& s, std::uint16_t locate) {
  if (locate < s.symbols.size() && !s.symbols[locate].empty()) return s.symbols[locate];
  return "locate " + std::to_string(locate);
}

inline std::uint64_t symbol_count(const itch::FileSummary& s) {
  return static_cast<std::uint64_t>(
      std::count_if(s.symbols.begin(), s.symbols.end(), [](const auto& n) { return !n.empty(); }));
}

// A feed cannot hold more live orders than it has adds and replaces, so small files get small
// tables and a full day is capped at 8M live orders.
inline book::FastBookConfig fast_config(const itch::FileSummary& s, std::uint64_t max_orders) {
  const std::uint64_t creators = s.of(itch::AddOrder::kType) + s.of(itch::AddOrderMpid::kType) +
                                 s.of(itch::OrderReplace::kType);
  const std::uint64_t orders =
      max_orders != 0 ? max_orders : std::min<std::uint64_t>(creators + 1, std::uint64_t{1} << 23);
  book::FastBookConfig c;
  c.num_locates = s.max_locate + 1;
  c.max_orders =
      static_cast<std::uint32_t>(std::min<std::uint64_t>(orders, std::uint64_t{1} << 30));
  c.far_arena_bytes = static_cast<std::size_t>(
      std::clamp<std::uint64_t>(creators * 96, std::uint64_t{16} << 20, std::uint64_t{256} << 20));
  return c;
}

inline void print_file(std::string_view path, std::uint64_t file_size, const itch::FileSummary& s) {
  std::printf("file         %.*s\n", static_cast<int>(path.size()), path.data());
  std::printf("size         %s, %s messages, %s symbols\n", bytes_text(file_size).c_str(),
              commas(s.messages).c_str(), commas(symbol_count(s)).c_str());
  if (s.bytes != file_size) {
    std::printf("warning      ignoring %s trailing bytes that do not form a full message\n",
                commas(file_size - s.bytes).c_str());
  }
  if (s.malformed != 0) {
    std::printf("warning      %s messages are shorter than the spec allows\n",
                commas(s.malformed).c_str());
  }
}

}  // namespace tools
