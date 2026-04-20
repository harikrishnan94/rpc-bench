// Server-side hash-service bootstrap. The application and transport layers use
// this capability to serve the CRC32 RPC over async streams.

#include "server/server.hpp"

#include "protocol/crc32.hpp"
#include "protocol/hash_service.capnp.h"
#include "protocol/service.hpp"

#include <kj/debug.h>
#include <span>

namespace rpcbench {
namespace {

[[nodiscard]] std::span<const std::byte> to_byte_span(capnp::Data::Reader payload) {
  return {
      reinterpret_cast<const std::byte*>(payload.begin()),
      payload.size(),
  };
}

class HashServiceImpl final : public HashService::Server {
  // Implements the benchmark's one unary RPC. The method keeps the 1 MiB
  // payload contract in the service layer so oversized direct RPC callers fail
  // cleanly even outside the benchmark CLI.
public:
  kj::Promise<void> hash(HashContext context) override {
    const auto payload = context.getParams().getPayload();
    KJ_REQUIRE(payload.size() <= kMaxPayloadSizeBytes,
               "payload exceeds maximum size",
               payload.size(),
               kMaxPayloadSizeBytes);

    context.getResults().setCrc32(compute_crc32(to_byte_span(payload)));
    co_return;
  }
};

[[nodiscard]] capnp::Capability::Client make_bootstrap_service() {
  return kj::heap<HashServiceImpl>();
}

} // namespace

capnp::Capability::Client make_hash_service_bootstrap() {
  return make_bootstrap_service();
}

} // namespace rpcbench
