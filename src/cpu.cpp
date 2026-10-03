#include "util/cpu.hpp"

#include <chrono>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace util {

double tsc_ticks_per_ns() {
  static const double ticks_per_ns = [] {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    const std::uint64_t c0 = tsc_now();
    while (Clock::now() - t0 < std::chrono::milliseconds(250)) {
    }
    const auto t1 = Clock::now();
    const std::uint64_t c1 = tsc_now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return static_cast<double>(c1 - c0) / static_cast<double>(ns);
  }();
  return ticks_per_ns;
}

bool pin_thread(int cpu) {
  if (cpu < 0) return false;
#if defined(_WIN32)
  if (cpu >= 64) return false;
  return SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu) != 0;
#elif defined(__linux__)
  if (cpu >= CPU_SETSIZE) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<std::size_t>(cpu), &set);
  return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
#else
  return false;
#endif
}

}  // namespace util
