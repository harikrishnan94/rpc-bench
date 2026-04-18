// CRC32 implementation for the benchmark protocol. Keeping the lookup table in
// the repository avoids introducing another hashing dependency while preserving
// deterministic byte-for-byte behavior.

#include "protocol/crc32.hpp"

#include <array>

namespace rpcbench {
namespace {

constexpr std::uint32_t kCrc32Polynomial = 0xedb88320U;

consteval std::array<std::uint32_t, 256> make_crc32_table() {
  std::array<std::uint32_t, 256> table{};

  for (std::uint32_t index = 0; index < table.size(); ++index) {
    std::uint32_t value = index;
    for (int round = 0; round < 8; ++round) {
      const bool set_lsb = (value & 1U) != 0U;
      value >>= 1U;
      if (set_lsb) {
        value ^= kCrc32Polynomial;
      }
    }
    table[index] = value;
  }

  return table;
}

constexpr auto kCrc32Table = make_crc32_table();

} // namespace

std::uint32_t compute_crc32(std::span<const std::byte> payload) {
  std::uint32_t crc = 0xffffffffU;

  for (const auto byte : payload) {
    const auto index =
        static_cast<std::uint8_t>((crc ^ std::to_integer<std::uint8_t>(byte)) & 0xffU);
    crc = (crc >> 8U) ^ kCrc32Table[index];
  }

  return ~crc;
}

} // namespace rpcbench
