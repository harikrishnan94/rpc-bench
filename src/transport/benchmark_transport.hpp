#pragma once

// Benchmark-side transport orchestration. This layer owns spawn-local process
// setup, worker attachment creation, and shared-memory rendezvous so the
// benchmark runtime can stay focused on workload generation and reporting.

#include "transport/rpc_session.hpp"
#include "transport/uri.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <vector>

namespace rpcbench {

class PathCleanupGuard;
class ShmBenchmarkTransport;
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

  // One client thread and one transport attachment per worker.
  std::size_t client_threads = 1;

  // Suppresses the child server banner in spawn-local mode.
  bool quiet_server = false;

  // Timeout while waiting for a spawned local endpoint to come up.
  std::uint32_t startup_timeout_ms = 5000;
};

class WorkerAttachment {
  // Move-only transport attachment for one benchmark worker. Each attachment
  // opens exactly one channel so preconnected pipe ends and shared-memory slots
  // cannot be accidentally reused across workers.
public:
  WorkerAttachment() = default;
  WorkerAttachment(const WorkerAttachment&) = delete;
  WorkerAttachment& operator=(const WorkerAttachment&) = delete;
  WorkerAttachment(WorkerAttachment&& other) noexcept = default;
  WorkerAttachment& operator=(WorkerAttachment&& other) noexcept = default;

  [[nodiscard]] std::expected<RpcChannel, std::string>
  open_channel(const TransportUri& uri, kj::AsyncIoContext& io_context) &&;

private:
  friend class PreparedBenchmarkTransport;

  enum class Kind : std::uint8_t {
    network,
    preconnected_stream,
    shared_memory,
  };

  explicit WorkerAttachment(kj::AutoCloseFd&& preconnected_fd);
  WorkerAttachment(ShmBenchmarkTransport* shm_transport, std::size_t slot_index);

  Kind kind_ = Kind::network;
  kj::AutoCloseFd preconnected_fd_;
  ShmBenchmarkTransport* shm_transport_ = nullptr;
  std::size_t shm_slot_index_ = 0;
};

class PreparedBenchmarkTransport {
  // Owns the transport-specific state for one benchmark invocation, including
  // any spawned child server, inherited descriptors, and shared-memory control
  // channels.
public:
  static std::expected<PreparedBenchmarkTransport, std::string>
  prepare(const BenchmarkTransportConfig& config);

  PreparedBenchmarkTransport();
  ~PreparedBenchmarkTransport();
  PreparedBenchmarkTransport(const PreparedBenchmarkTransport&) = delete;
  PreparedBenchmarkTransport& operator=(const PreparedBenchmarkTransport&) = delete;
  PreparedBenchmarkTransport(PreparedBenchmarkTransport&& other) noexcept;
  PreparedBenchmarkTransport& operator=(PreparedBenchmarkTransport&& other) noexcept;

  [[nodiscard]] TransportUri resolved_uri() const;
  [[nodiscard]] std::optional<std::string> background_error() const;
  std::vector<WorkerAttachment> take_worker_attachments();

private:
  TransportUri resolved_uri_;
  std::vector<WorkerAttachment> worker_attachments_;
  std::unique_ptr<PathCleanupGuard> cleanup_guard_;
  std::unique_ptr<SpawnedProcess> spawned_server_;
  std::unique_ptr<ShmBenchmarkTransport> shm_transport_;
};

} // namespace rpcbench
