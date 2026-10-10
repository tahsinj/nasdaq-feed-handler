#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace itch {

struct FileSummary {
  std::uint64_t messages = 0;
  std::uint64_t bytes = 0;      // length of the prefix made of complete messages
  std::uint64_t malformed = 0;  // known types shorter than the spec length
  std::array<std::uint64_t, 256> count{};
  std::uint32_t max_locate = 0;
  std::vector<std::string> symbols;  // indexed by stock locate

  std::uint64_t of(char type) const noexcept { return count[static_cast<unsigned char>(type)]; }
};

// One pass over the file: message counts, symbol directory and the highest stock locate.
// Throws if the data is still gzip-compressed.
FileSummary summarize(const char* data, std::size_t size);

// Spec length of a message type, or 0 if the type is not part of ITCH 5.0.
std::size_t spec_size(char type) noexcept;

const char* message_name(char type) noexcept;

}  // namespace itch
