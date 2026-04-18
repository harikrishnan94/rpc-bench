#pragma once

// Benchmark execution entry points. The runner expands the configured sweep
// matrix, manages remote or spawn-local endpoints, and returns fully formatted
// metric structures without exposing transport details to the frontends.

#include "rpcbench/config.hpp"
#include "rpcbench/metrics.hpp"

#include <expected>

namespace rpcbench {

class BenchmarkRunner {
public:
  // Captures the benchmark configuration for later execution.
  explicit BenchmarkRunner(BenchConfig config);

  // Executes the full benchmark matrix and returns the aggregated report or a
  // user-facing error string.
  [[nodiscard]] std::expected<BenchmarkReport, std::string> run() const;

private:
  BenchConfig config_;
};

} // namespace rpcbench
