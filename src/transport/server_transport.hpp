#pragma once

// Server-side transport runtime. This layer owns the KJ event loop, listener
// setup, and per-transport session plumbing so the CLI-facing server frontend
// only needs to parse options and provide the bootstrap capability.

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

  // Suppresses the startup banner when true.
  bool quiet = false;

  // Internal ready notification fd used by spawn-local mode.
  std::optional<int> ready_fd;

  // Internal inherited server-side stream fds for pipe://socketpair.
  std::vector<int> preconnected_stream_fds;

  // Fixed shared-memory slot capacity for shm://NAME.
  std::size_t shm_slot_count = 0;
};

// Runs the full transport-specific server lifecycle for one listen target.
int run_server_transport(const ServerTransportConfig& config, capnp::Capability::Client bootstrap);

} // namespace rpcbench
