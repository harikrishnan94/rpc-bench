#pragma once

// Benchmark-side transport orchestration. This layer owns URI-aware
// spawn-local setup and per-connection opener construction so the benchmark
// runtime itself only deals with async stream openers.

#include "bench/benchmark.hpp"
#include "transport/uri.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace rpcbench {

class PathCleanupGuard;
class SpawnedProcess;

struct BenchmarkTransportConfig {
  // Selects whether the benchmark attaches to an existing target or spawns one
  // local child server first.
  BenchMode mode = BenchMode::spawn_local;

  // URI for connect mode.
  std::optional<TransportUri> connect_uri;

  // Path to the server binary used by spawn-local mode.
  std::filesystem::path server_binary;

  // URI for the spawned local server.
  TransportUri listen_uri{
      .kind = TransportKind::tcp,
      .location = "127.0.0.1",
      .port = 7300,
  };

  // One client stream opener per logical benchmark connection.
  std::size_t client_connections = 1;

  // Requested server thread count forwarded only in spawn-local mode.
  std::size_t server_threads = 1;

  // Suppresses the child server banner in spawn-local mode.
  bool quiet_server = false;

  // Timeout while waiting for a spawned local endpoint to come up.
  std::uint32_t startup_timeout_ms = 5000;
};

class PreparedBenchmarkTransport {
  // Owns the transport-specific state for one benchmark invocation, including
  // any spawned child server and one async stream opener per logical client
  // connection.
public:
  static std::expected<PreparedBenchmarkTransport, std::string>
  prepare(const BenchmarkTransportConfig& config);

  PreparedBenchmarkTransport();
  ~PreparedBenchmarkTransport();
  PreparedBenchmarkTransport(const PreparedBenchmarkTransport&) = delete;
  PreparedBenchmarkTransport& operator=(const PreparedBenchmarkTransport&) = delete;
  PreparedBenchmarkTransport(PreparedBenchmarkTransport&& other) noexcept;
  PreparedBenchmarkTransport& operator=(PreparedBenchmarkTransport&& other) noexcept;

  [[nodiscard]] std::string resolved_uri() const;
  std::vector<kj::Own<ConnectionOpener>> take_connection_openers();

private:
  std::string resolved_uri_;
  std::vector<kj::Own<ConnectionOpener>> connection_openers_;
  std::unique_ptr<PathCleanupGuard> cleanup_guard_;
  std::unique_ptr<SpawnedProcess> spawned_server_;
};

} // namespace rpcbench
