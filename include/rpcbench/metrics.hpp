#pragma once

// Benchmark result types plus text and JSON formatting helpers. The benchmark
// runner fills these structures, while the frontends and tests rely on the
// stable formatting methods to present and validate results.

#include "rpcbench/config.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rpcbench {

struct OperationCounts {
  // Total number of successfully completed benchmark operations.
  std::uint64_t total_ops = 0;

  // Per-method counts for the completed operations.
  std::uint64_t get_ops = 0;
  std::uint64_t put_ops = 0;
  std::uint64_t delete_ops = 0;

  // Outcome counts for methods whose result can be absent.
  std::uint64_t found_gets = 0;
  std::uint64_t missing_gets = 0;
  std::uint64_t removed_deletes = 0;
  std::uint64_t missing_deletes = 0;

  // Count of failed RPCs or local benchmark errors.
  std::uint64_t errors = 0;

  // Logical payload bytes transferred in requests and responses.
  std::uint64_t request_bytes = 0;
  std::uint64_t response_bytes = 0;
};

struct LatencyPercentiles {
  // Latency samples expressed in nanoseconds.
  std::uint64_t min_ns = 0;
  std::uint64_t p50_ns = 0;
  std::uint64_t p90_ns = 0;
  std::uint64_t p99_ns = 0;
  std::uint64_t max_ns = 0;
};

struct BenchmarkResult {
  // Server worker count used for this run. In v1 this equals the number of
  // endpoints or spawned server processes.
  std::size_t server_workers = 0;

  // Client thread count used for this run.
  std::size_t client_threads = 0;

  // In-flight request count per client thread.
  std::size_t queue_depth = 0;

  // Key and value sizes used by the workload.
  std::size_t key_size = 0;
  std::size_t value_size = 0;

  // Operation mix used by the workload.
  OperationMix mix;

  // One-based iteration number within the matrix point.
  std::uint32_t iteration = 0;

  // Measured duration, in seconds, for the recorded phase.
  double measure_seconds = 0.0;

  // Endpoints used by the run.
  std::vector<std::string> endpoints;

  // Aggregated operation counts for the run.
  OperationCounts counts;

  // Latency summary across all measured operations in the run.
  LatencyPercentiles latency;

  // Operations per second for the measured phase.
  double ops_per_second = 0.0;

  // Payload throughput in MiB/s for the measured phase.
  double mib_per_second = 0.0;
};

struct BenchmarkReport {
  // Mode that produced this report.
  BenchMode mode = BenchMode::spawn_local;

  // All measured results in the order they were executed.
  std::vector<BenchmarkResult> results;

  // Formats the report for terminal output.
  [[nodiscard]] std::string to_text() const;

  // Formats the report as JSON for automation.
  [[nodiscard]] std::string to_json() const;
};

} // namespace rpcbench
