#pragma once

// Server frontend for the CRC32 benchmark. This layer owns the CLI-facing
// configuration and hash-service bootstrap while the transport module owns the
// blocking KJ event-loop lifecycle for each listen target.

#include "transport/uri.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rpcbench {

struct ServerConfig {
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

// Parses `rpc-bench-server` command-line arguments.
[[nodiscard]] std::expected<ServerConfig, std::string>
parse_server_config(std::span<const std::string_view> args);

// Returns the usage text for `rpc-bench-server`.
[[nodiscard]] std::string server_usage(std::string_view program_name);

class ServerApp {
  // Owns the blocking server lifecycle. One instance runs one listener and one
  // KJ event loop until shutdown is requested.
public:
  explicit ServerApp(ServerConfig config);

  // Blocks until the server is interrupted or fails to start.
  int run();

private:
  ServerConfig config_;
};

} // namespace rpcbench
