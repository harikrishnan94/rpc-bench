#pragma once

// CLI parsing plus top-level application orchestration for both executables.
// This layer is the only one that knows URI syntax and run modes; the bench
// and server runtime layers operate on typed async stream abstractions instead.

#include "bench/benchmark.hpp"
#include "transport/uri.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
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

  // Requested server thread count. Values greater than `1` are parsed but
  // rejected by the server runtime itself so spawn-local failures come from the
  // child server process rather than from the benchmark CLI.
  std::size_t server_threads = 1;

  // Suppresses the startup banner when true.
  bool quiet = false;

  // Internal ready notification fd used by spawn-local mode.
  std::optional<int> ready_fd;

  // Internal inherited server-side stream fds for pipe://socketpair.
  std::vector<int> preconnected_stream_fds;
};

struct BenchConfig {
  // Selects whether the benchmark connects to an existing server or spawns one
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

  // Requested server thread count forwarded only in spawn-local mode.
  std::size_t server_threads = 1;

  // Benchmark runtime options shared by both connect and spawn-local modes.
  BenchmarkOptions benchmark;

  // Optional JSON report path.
  std::optional<std::filesystem::path> json_output;

  // Suppresses the child server banner in spawn-local mode.
  bool quiet_server = false;

  // Timeout while waiting for a spawned local endpoint to come up.
  std::uint32_t startup_timeout_ms = 5000;
};

// Parses `rpc-bench-server` command-line arguments.
[[nodiscard]] std::expected<ServerConfig, std::string>
parse_server_config(std::span<const std::string_view> args);

// Returns the usage text for `rpc-bench-server`.
[[nodiscard]] std::string server_usage(std::string_view program_name);

// Runs the full server application after parsing succeeded.
int run_server_app(const ServerConfig& config);

// Parses `rpc-bench-bench` command-line arguments and resolves the default
// server binary path from the benchmark executable's location.
[[nodiscard]] std::expected<BenchConfig, std::string>
parse_bench_config(std::span<const std::string_view> args, const std::filesystem::path& argv0);

// Returns the usage text for `rpc-bench-bench`.
[[nodiscard]] std::string bench_usage(std::string_view program_name);

// Runs the full benchmark application after parsing succeeded.
[[nodiscard]] std::expected<BenchmarkResult, std::string>
run_benchmark_app(const BenchConfig& config);

} // namespace rpcbench
