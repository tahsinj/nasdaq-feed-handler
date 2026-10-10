#pragma once

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#define FH_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#else
#include <chrono>
#endif

namespace util {

#if defined(FH_X86)

// The fences keep the timed instructions from moving outside the two counter reads.
inline std::uint64_t tsc_begin() noexcept {
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
}

inline std::uint64_t tsc_end() noexcept {
  unsigned aux;
  const std::uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
}

inline std::uint64_t tsc_now() noexcept { return __rdtsc(); }

inline void cpu_relax() noexcept { _mm_pause(); }

#else

inline std::uint64_t tsc_now() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}
inline std::uint64_t tsc_begin() noexcept { return tsc_now(); }
inline std::uint64_t tsc_end() noexcept { return tsc_now(); }
inline void cpu_relax() noexcept {}

#endif

// Ticks from `start` to `end`, read on possibly different cores. Returns 0 when `end`
// reads behind `start`, which happens when the cores' counters are slightly out of sync.
inline std::uint64_t elapsed_ticks(std::uint64_t start, std::uint64_t end) noexcept {
  return end > start ? end - start : 0;
}

// Counter ticks per nanosecond, calibrated once against steady_clock.
double tsc_ticks_per_ns();

// Pins the calling thread to one logical CPU. Returns false if the OS refuses or cannot pin.
bool pin_thread(int cpu);

}  // namespace util
