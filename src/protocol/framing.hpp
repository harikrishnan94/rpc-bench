#pragma once

// Small helpers for the fixed-width integer fields used by the framed TCP
// protocol. The server and benchmark both use these helpers to keep request
// lengths and CRC32 replies in stable network byte order.

#include <array>
#include <cstddef>
#include <cstdint>

namespace rpcbench {

inline constexpr std::size_t kFrameHeaderBytes = 4;

// Encodes one unsigned 32-bit integer in big-endian network byte order.
[[nodiscard]] inline std::array<std::byte, kFrameHeaderBytes> encode_be32(std::uint32_t value) {
  return {
      std::byte((value >> 24U) & 0xffU),
      std::byte((value >> 16U) & 0xffU),
      std::byte((value >> 8U) & 0xffU),
      std::byte(value & 0xffU),
  };
}

// Decodes one unsigned 32-bit integer from big-endian network byte order.
[[nodiscard]] inline std::uint32_t
decode_be32(const std::array<std::byte, kFrameHeaderBytes>& bytes) {
  return (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[0])) << 24U) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[1])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[2])) << 8U) |
         static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[3]));
}

} // namespace rpcbench
