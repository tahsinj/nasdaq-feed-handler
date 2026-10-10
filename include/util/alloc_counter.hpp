#pragma once

#include <cstdint>

namespace util {

// Calls to global operator new so far. Counted by binaries that link src/alloc_counter.cpp,
// which replaces the global allocation functions.
std::uint64_t heap_allocations() noexcept;

}  // namespace util
