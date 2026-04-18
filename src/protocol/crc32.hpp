#pragma once

// CRC32 helpers for the framed TCP benchmark protocol. The server uses this to
// hash request payloads, and the benchmark uses the same function for optional
// local cross-checking and stable documentation of the wire semantics.

#include <cstddef>
#include <cstdint>
#include <span>

namespace rpcbench {

// Computes the IEEE CRC32 value for the given payload bytes.
[[nodiscard]] std::uint32_t compute_crc32(std::span<const std::byte> payload);

} // namespace rpcbench
