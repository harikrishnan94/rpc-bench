#pragma once

// CRC32 helpers for the benchmark service. The server uses this to hash RPC
// payloads, and the benchmark reuses the same function in tests and docs to
// keep the service contract stable.

#include <cstddef>
#include <cstdint>
#include <span>

namespace rpcbench {

// Computes the IEEE CRC32 value for the given payload bytes.
[[nodiscard]] std::uint32_t compute_crc32(std::span<const std::byte> payload);

} // namespace rpcbench
