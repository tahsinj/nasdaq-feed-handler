#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace util {

// Log-linear histogram: exact below 1024, then 64 buckets per power of two (under 1.6% error).
// Recording is two increments, so it can sit inside a timed loop over hundreds of millions of
// samples without storing them.
class Histogram {
 public:
  void record(std::uint64_t v) noexcept {
    ++counts_[bucket(v)];
    ++count_;
    sum_ += v;
    max_ = std::max(max_, v);
  }

  void merge(const Histogram& other) noexcept {
    for (std::size_t b = 0; b < kBuckets; ++b) counts_[b] += other.counts_[b];
    count_ += other.count_;
    sum_ += other.sum_;
    max_ = std::max(max_, other.max_);
  }

  std::uint64_t count() const noexcept { return count_; }
  std::uint64_t max() const noexcept { return max_; }
  double mean() const noexcept {
    return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
  }

  // Upper edge of the bucket holding the q-th quantile.
  std::uint64_t quantile(double q) const noexcept {
    if (count_ == 0) return 0;
    const auto rank = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(count_))));
    std::uint64_t seen = 0;
    for (std::size_t b = 0; b < kBuckets; ++b) {
      seen += counts_[b];
      if (seen >= rank) return std::min(upper_edge(b), max_);
    }
    return max_;
  }

 private:
  static constexpr std::uint64_t kLinear = 1024;
  static constexpr unsigned kLinearBits = 10;
  static constexpr unsigned kSubBits = 6;
  static constexpr std::size_t kBuckets =
      kLinear + (64 - kLinearBits) * (std::size_t{1} << kSubBits);

  static std::size_t bucket(std::uint64_t v) noexcept {
    if (v < kLinear) return static_cast<std::size_t>(v);
    const auto e = static_cast<unsigned>(std::bit_width(v)) - 1;
    const auto sub = static_cast<std::size_t>((v >> (e - kSubBits)) & 63);
    return static_cast<std::size_t>(kLinear) + (e - kLinearBits) * 64 + sub;
  }

  static std::uint64_t upper_edge(std::size_t b) noexcept {
    if (b < kLinear) return b;
    const std::size_t i = b - static_cast<std::size_t>(kLinear);
    const auto e = static_cast<unsigned>(i / 64) + kLinearBits;
    const std::uint64_t sub = i % 64;
    return ((64 + sub + 1) << (e - kSubBits)) - 1;
  }

  std::vector<std::uint64_t> counts_ = std::vector<std::uint64_t>(kBuckets);
  std::uint64_t count_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t max_ = 0;
};

}  // namespace util
