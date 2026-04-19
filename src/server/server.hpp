#pragma once

// Server frontend for the CRC32 benchmark. This layer owns the CLI-facing
// configuration and the blocking lifecycle around the single-threaded KJ event
// loop that hosts the Cap'n Proto RPC listener.

#include "protocol/common.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace rpcbench {

struct ServerConfig {
  // Host or address string to bind. The value is preserved for the startup
  // banner so operators can see the configured listen target directly.
  std::string listen_host = "127.0.0.1";

  // TCP port for the listener.
  std::uint16_t port = 7000;

  // Suppresses the startup banner when true.
  bool quiet = false;

  // Returns the configured listen endpoint.
  [[nodiscard]] Endpoint endpoint() const;
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
