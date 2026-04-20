#pragma once

// Hash-service bootstrap owned by the server runtime. The application and
// transport layers use this to serve the CRC32 RPC over async streams without
// depending on URI or CLI details here.

#include <capnp/capability.h>

namespace rpcbench {

// Returns a fresh bootstrap capability that implements the hash RPC.
[[nodiscard]] capnp::Capability::Client make_hash_service_bootstrap();

} // namespace rpcbench
