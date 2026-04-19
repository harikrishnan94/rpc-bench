#pragma once

// Benchmark frontend for the CRC32 RPC service. This layer owns CLI parsing,
// spawn-local process management, workload generation, and report rendering for
// one benchmark invocation.

#include "protocol/common.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace rpcbench {

struct BenchConfig {
  // Selects whether the benchmark connects to one existing endpoint or starts
  // one local child server automatically.
  BenchMode mode = BenchMode::spawn_local;

  // Endpoint for connect mode.
  Endpoint endpoint;

  // Path to the server binary used by spawn-local mode.
  std::filesystem::path server_binary;

  // Host and port for the spawned local server.
  std::string listen_host = "127.0.0.1";
  std::uint16_t server_port = 7300;

  // One client thread and one RPC connection per thread.
  std::size_t client_threads = 1;

  // Inclusive request payload size range.
  MessageSizeRange message_sizes;

  // Warmup and measured durations in seconds.
  double warmup_seconds = 1.0;
  double measure_seconds = 3.0;

  // Base seed for deterministic per-thread payload generation.
  std::uint64_t seed = 1;

  // Optional JSON report path.
  std::optional<std::filesystem::path> json_output;

  // Suppresses the child server banner in spawn-local mode.
  bool quiet_server = false;

  // Timeout while waiting for a spawned local endpoint to come up.
  std::uint32_t startup_timeout_ms = 5000;

  // Validates the mode-specific settings and size range.
  [[nodiscard]] std::expected<void, std::string> validate() const;

  // Returns the effective endpoint for this run.
  [[nodiscard]] Endpoint resolved_endpoint() const;
};

struct LatencyPercentiles {
  // RTT samples are expressed in nanoseconds.
  std::uint64_t p50_ns = 0;
  std::uint64_t p75_ns = 0;
  std::uint64_t p90_ns = 0;
  std::uint64_t p99_ns = 0;
  std::uint64_t p999_ns = 0;
};

struct BenchmarkResult {
  // Stable run identity.
  BenchMode mode = BenchMode::spawn_local;
  std::string endpoint;
  std::size_t client_threads = 0;
  MessageSizeRange message_sizes;

  // Measured traffic counters. These intentionally preserve the legacy logical
  // request/reply accounting used by earlier framed-TCP reports.
  std::uint64_t total_requests = 0;
  std::uint64_t errors = 0;
  std::uint64_t request_bytes = 0;
  std::uint64_t response_bytes = 0;

  // Throughput values derived from the measured phase duration.
  double measured_seconds = 0.0;
  double requests_per_second = 0.0;
  double request_mib_per_second = 0.0;
  double response_mib_per_second = 0.0;
  double combined_mib_per_second = 0.0;

  // RTT percentiles for successful measured requests.
  LatencyPercentiles latency;

  // Renders the human-readable terminal summary.
  [[nodiscard]] std::string to_text() const;

  // Renders the machine-readable JSON summary.
  [[nodiscard]] std::string to_json() const;
};

// Parses `rpc-bench-bench` command-line arguments.
[[nodiscard]] std::expected<BenchConfig, std::string>
parse_bench_config(std::span<const std::string_view> args, const std::filesystem::path& argv0);

// Returns the usage text for `rpc-bench-bench`.
[[nodiscard]] std::string bench_usage(std::string_view program_name);

// Executes one benchmark invocation and returns the aggregated result.
[[nodiscard]] std::expected<BenchmarkResult, std::string> run_benchmark(const BenchConfig& config);

} // namespace rpcbench
