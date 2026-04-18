#pragma once

// Configuration types and parsers for the runnable frontends. This header keeps
// CLI-facing policy in one place so the benchmark runner, tests, and entry
// points agree on validation and default behavior.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rpcbench {

enum class BenchMode : std::uint8_t {
  connect,
  spawn_local,
};

struct OperationMix {
  // Percentage of operations that issue a get request. The three percentages
  // together must add up to 100.
  std::uint32_t get_percent = 80;

  // Percentage of operations that issue a put request.
  std::uint32_t put_percent = 20;

  // Percentage of operations that issue a delete request.
  std::uint32_t delete_percent = 0;

  // Returns a stable human-readable label such as "80:20:0".
  [[nodiscard]] std::string label() const;
};

struct WorkloadSpec {
  // Client thread counts that the benchmark runner will sweep.
  std::vector<std::size_t> client_threads{1};

  // Maximum in-flight request counts per client thread.
  std::vector<std::size_t> queue_depths{1};

  // Key sizes, in bytes, for the sweep matrix.
  std::vector<std::size_t> key_sizes{16};

  // Value sizes, in bytes, for the sweep matrix.
  std::vector<std::size_t> value_sizes{128};

  // Operation mixes to execute. Each entry represents one benchmark profile.
  std::vector<OperationMix> mixes{OperationMix{}};

  // Number of logical keys each client thread owns during a run.
  std::size_t key_space = 4096;

  // Warmup duration in seconds. Warmup traffic is not included in the report.
  double warmup_seconds = 1.0;

  // Measurement duration in seconds for each matrix point.
  double measure_seconds = 3.0;

  // Number of repeated measured runs for each matrix point.
  std::uint32_t iterations = 1;

  // Base seed used to derive deterministic per-thread workload streams.
  std::uint64_t seed = 1;

  // Validates sweep sizes, durations, percentages, and iteration counts.
  [[nodiscard]] std::expected<void, std::string> validate() const;
};

struct ServerConfig {
  // Host or address string that EzRpc should bind to.
  std::string listen_host = "127.0.0.1";

  // TCP port that the server process listens on.
  std::uint16_t port = 7000;

  // Suppresses the startup banner when true.
  bool quiet = false;

  // Returns the address string passed to EzRpc.
  [[nodiscard]] std::string bind_address() const;
};

struct BenchConfig {
  // Selects whether the benchmark connects to existing endpoints or spawns
  // local server processes automatically.
  BenchMode mode = BenchMode::spawn_local;

  // Endpoints used by connect mode. Each endpoint owns one shard.
  std::vector<std::string> endpoints;

  // Path to the server binary used by spawn-local mode.
  std::filesystem::path server_binary = "rpc-bench-server";

  // Host used for spawn-local server processes.
  std::string listen_host = "127.0.0.1";

  // First port used for spawn-local server processes. Additional workers use
  // consecutive ports.
  std::uint16_t base_port = 7300;

  // Server worker counts to sweep in spawn-local mode. Each worker count maps
  // to one server process per endpoint.
  std::vector<std::size_t> server_workers{1};

  // Benchmark matrix, timing, and workload settings.
  WorkloadSpec workload;

  // Optional file path for JSON report emission.
  std::optional<std::filesystem::path> json_output;

  // Suppresses child-process server logs in spawn-local mode when true.
  bool quiet_server = false;

  // Deadline for waiting on spawned endpoints to become reachable.
  std::uint32_t startup_timeout_ms = 5000;

  // Validates the mode-specific fields and embedded workload configuration.
  [[nodiscard]] std::expected<void, std::string> validate() const;
};

// Parses `rpc-bench-server` command-line arguments.
[[nodiscard]] std::expected<ServerConfig, std::string>
parse_server_config(std::span<const std::string_view> args);

// Parses `rpc-bench-bench` command-line arguments and resolves the default
// server binary path from the benchmark executable's location.
[[nodiscard]] std::expected<BenchConfig, std::string>
parse_bench_config(std::span<const std::string_view> args, const std::filesystem::path& argv0);

// Returns usage text for `rpc-bench-server`.
[[nodiscard]] std::string server_usage(std::string_view program_name);

// Returns usage text for `rpc-bench-bench`.
[[nodiscard]] std::string bench_usage(std::string_view program_name);

} // namespace rpcbench
