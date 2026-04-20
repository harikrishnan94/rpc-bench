#pragma once

// Service-contract constants shared by the server, benchmark, and transport
// layers. Keeping them here lets the transport refactor stay faithful to the
// externally visible payload contract without coupling that contract to URI or
// process-management helpers.

#include <cstdint>

namespace rpcbench {

inline constexpr std::uint32_t kMaxPayloadSizeBytes = 1024U * 1024U;

} // namespace rpcbench
