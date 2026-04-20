#pragma once

// Benchmark runtime for the CRC32 RPC service. This layer owns workload
// generation, coroutine-based client execution, and report rendering while the
// application layer handles CLI parsing and transport orchestration.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <kj/async-io.h>
#include <string>
#include <vector>

namespace rpcbench {

inline constexpr std::size_t kDefaultMessageSizeMin = 128;
inline constexpr std::size_t kDefaultMessageSizeMax = 256;

struct MessageSizeRange {
  // Inclusive lower and upper request-payload bounds in bytes. Zero-length
  // payloads are allowed when explicitly requested, but the range may never
  // exceed the service's 1 MiB payload limit.
  std::size_t min = kDefaultMessageSizeMin;
  std::size_t max = kDefaultMessageSizeMax;

  // Validates ordering and the hard payload cap.
  [[nodiscard]] std::expected<void, std::string> validate() const;
};

struct BenchmarkOptions {
  // Number of benchmark event-loop threads. Each thread owns one KJ event loop
  // and hosts zero or more client connections.
  std::size_t client_threads = 1;

  // Total number of client RPC connections spread as evenly as possible across
  // the configured event-loop threads.
  std::size_t client_connections = 1;

  // Inclusive request payload size range.
  MessageSizeRange message_sizes;

  // Warmup and measured durations in seconds.
  double warmup_seconds = 1.0;
  double measure_seconds = 3.0;

  // Base seed for deterministic per-thread payload generation.
  std::uint64_t seed = 1;
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
  std::string mode;
  std::string endpoint;
  std::size_t client_threads = 0;
  std::size_t client_connections = 0;
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

class ConnectionOpener {
  // Opens exactly one client-side stream-backed RPC connection. Transport
  // orchestration implements this interface so the benchmark runtime can stay
  // focused on async request execution rather than URI or spawn-local details.
public:
  virtual ~ConnectionOpener() = default;

  [[nodiscard]] virtual kj::Promise<kj::Own<kj::AsyncIoStream>>
  open(kj::AsyncIoContext& io_context) = 0;
};

struct BenchmarkRunInput {
  // Stable run identity strings chosen by the application layer.
  std::string mode;
  std::string endpoint;

  // Runtime options and one opener per logical connection.
  BenchmarkOptions options;
  std::vector<kj::Own<ConnectionOpener>> connection_openers;
};

// Executes one benchmark invocation and returns the aggregated result.
[[nodiscard]] std::expected<BenchmarkResult, std::string> run_benchmark(BenchmarkRunInput input);

} // namespace rpcbench
