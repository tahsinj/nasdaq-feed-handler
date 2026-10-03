#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#include <stdlib.h>
#endif

namespace itch {

static_assert(std::endian::native == std::endian::little, "decoding assumes a little-endian host");

inline std::uint16_t bswap16(std::uint16_t v) noexcept {
#if defined(_MSC_VER)
  return _byteswap_ushort(v);
#else
  return __builtin_bswap16(v);
#endif
}

inline std::uint32_t bswap32(std::uint32_t v) noexcept {
#if defined(_MSC_VER)
  return _byteswap_ulong(v);
#else
  return __builtin_bswap32(v);
#endif
}

inline std::uint64_t bswap64(std::uint64_t v) noexcept {
#if defined(_MSC_VER)
  return _byteswap_uint64(v);
#else
  return __builtin_bswap64(v);
#endif
}

// memcpy keeps unaligned reads well defined; compilers lower each one to a single load.
inline std::uint16_t load_be16(const char* p) noexcept {
  std::uint16_t v;
  std::memcpy(&v, p, sizeof v);
  return bswap16(v);
}

inline std::uint32_t load_be32(const char* p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof v);
  return bswap32(v);
}

inline std::uint64_t load_be64(const char* p) noexcept {
  std::uint64_t v;
  std::memcpy(&v, p, sizeof v);
  return bswap64(v);
}

inline std::uint64_t load_be48(const char* p) noexcept {
  return (std::uint64_t{load_be16(p)} << 32) | load_be32(p + 2);
}

}  // namespace itch
