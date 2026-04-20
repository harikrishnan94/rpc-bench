#pragma once

// Server-side transport runtime. This layer owns URI-aware listener setup and
// stream session plumbing so the application layer only provides validated
// config plus the bootstrap capability.

#include "transport/uri.hpp"

#include <capnp/capability.h>
#include <cstddef>
#include <optional>
#include <vector>

namespace rpcbench {

struct ServerTransportConfig {
  // URI that defines the server transport and listen target.
  TransportUri listen_uri{
      .kind = TransportKind::tcp,
      .location = "127.0.0.1",
      .port = 7000,
  };

  // Requested server thread count. Values greater than `1` are rejected here
  // so the error comes from the server process itself.
  std::size_t server_threads = 1;

  // Suppresses the startup banner when true.
  bool quiet = false;

  // Internal ready notification fd used by spawn-local mode.
  std::optional<int> ready_fd;

  // Internal inherited server-side stream fds for pipe://socketpair.
  std::vector<int> preconnected_stream_fds;
};

// Runs the full transport-specific server lifecycle for one listen target.
int run_server_transport(const ServerTransportConfig& config, capnp::Capability::Client bootstrap);

} // namespace rpcbench
