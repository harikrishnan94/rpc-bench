// Benchmark-side transport orchestration. This implementation keeps process
// management, inherited-descriptor setup, and shared-memory rendezvous out of
// the benchmark hot path so worker threads can open one RPC channel each
// without caring how the transport was provisioned.

#include "transport/benchmark_transport.hpp"

#include "transport/shm_transport.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <format>
#include <memory>
#include <poll.h>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

namespace rpcbench {
namespace {

using Clock = std::chrono::steady_clock;

struct ReadyPipe {
  // Spawn-local mode uses this pipe to learn when the child server finished
  // transport-specific startup and is ready for the benchmark parent.
  kj::AutoCloseFd read_end;
  kj::AutoCloseFd write_end;
};

struct SocketpairWorkers {
  // pipe://socketpair mode allocates one parent-side and one child-side socket
  // per worker before the child server is exec'd.
  std::vector<kj::AutoCloseFd> client_fds;
  std::vector<kj::AutoCloseFd> server_fds;
};

[[nodiscard]] std::expected<void, std::string> set_fd_inheritable(int fd, bool inheritable) {
  const int current_flags = ::fcntl(fd, F_GETFD);
  if (current_flags < 0) {
    return std::unexpected(std::format("fcntl(F_GETFD) failed for fd {}", fd));
  }

  const int next_flags = inheritable ? (current_flags & ~FD_CLOEXEC) : (current_flags | FD_CLOEXEC);
  if (::fcntl(fd, F_SETFD, next_flags) < 0) {
    return std::unexpected(std::format("fcntl(F_SETFD) failed for fd {}", fd));
  }
  return {};
}

[[nodiscard]] std::expected<ReadyPipe, std::string> create_ready_pipe() {
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    return std::unexpected("pipe() failed while creating the spawn-local ready pipe");
  }

  ReadyPipe pipe{
      .read_end = kj::AutoCloseFd(fds[0]),
      .write_end = kj::AutoCloseFd(fds[1]),
  };
  if (auto cloexec = set_fd_inheritable(pipe.read_end.get(), false); !cloexec) {
    return std::unexpected(cloexec.error());
  }
  if (auto cloexec = set_fd_inheritable(pipe.write_end.get(), false); !cloexec) {
    return std::unexpected(cloexec.error());
  }
  if (auto inheritable = set_fd_inheritable(pipe.write_end.get(), true); !inheritable) {
    return std::unexpected(inheritable.error());
  }
  return pipe;
}

[[nodiscard]] std::expected<void, std::string>
wait_for_ready_pipe(int fd, std::chrono::milliseconds timeout) {
  pollfd descriptor{
      .fd = fd,
      .events = POLLIN,
      .revents = 0,
  };
  const int poll_result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
  if (poll_result < 0) {
    return std::unexpected("poll() failed while waiting for the spawn-local ready pipe");
  }
  if (poll_result == 0) {
    return std::unexpected("spawn-local server did not report readiness before the timeout");
  }

  std::byte ready{};
  const auto bytes_read = ::read(fd, &ready, 1);
  if (bytes_read == 1) {
    return {};
  }
  if (bytes_read == 0) {
    return std::unexpected("spawn-local server exited before reporting readiness");
  }
  return std::unexpected("read() failed while waiting for the spawn-local ready pipe");
}

[[nodiscard]] std::expected<pid_t, std::string>
spawn_process(const std::filesystem::path& program, const std::vector<std::string>& args) {
  const auto program_string = program.string();
  if (!std::filesystem::exists(program)) {
    return std::unexpected(std::format("server binary does not exist: {}", program_string));
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    return std::unexpected(std::format("fork() failed while starting {}", program_string));
  }

  if (pid == 0) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program_string.c_str()));
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(program_string.c_str(), argv.data());
    _exit(127);
  }

  return pid;
}

[[nodiscard]] std::expected<SocketpairWorkers, std::string>
create_socketpair_workers(std::size_t count) {
  SocketpairWorkers workers;
  workers.client_fds.reserve(count);
  workers.server_fds.reserve(count);

  for (std::size_t index = 0; index < count; ++index) {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      return std::unexpected("socketpair() failed while preparing pipe://socketpair workers");
    }

    kj::AutoCloseFd client_end(fds[0]);
    kj::AutoCloseFd server_end(fds[1]);
    if (auto cloexec = set_fd_inheritable(client_end.get(), false); !cloexec) {
      return std::unexpected(cloexec.error());
    }
    if (auto cloexec = set_fd_inheritable(server_end.get(), false); !cloexec) {
      return std::unexpected(cloexec.error());
    }
    if (auto inheritable = set_fd_inheritable(server_end.get(), true); !inheritable) {
      return std::unexpected(inheritable.error());
    }

    workers.server_fds.emplace_back(kj::mv(server_end));
    workers.client_fds.emplace_back(kj::mv(client_end));
  }

  return workers;
}

[[nodiscard]] kj::Own<kj::AsyncIoStream> connect_network_stream(const TransportUri& uri,
                                                                kj::AsyncIoContext& io_context) {
  auto address_text = uri.to_kj_address();
  if (!address_text) {
    KJ_FAIL_REQUIRE("invalid KJ network address", address_text.error().c_str());
  }
  auto address = io_context.provider->getNetwork()
                     .parseAddress(address_text->c_str())
                     .wait(io_context.waitScope);
  return address->connect().wait(io_context.waitScope);
}

[[nodiscard]] std::expected<kj::AutoCloseFd, std::string>
connect_sidecar_socket(const std::filesystem::path& path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return std::unexpected("socket() failed while connecting to the shared-memory sidecar");
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto native = path.native();
  if (native.size() >= sizeof(address.sun_path)) {
    ::close(fd);
    return std::unexpected(
        std::format("shared-memory sidecar path is too long: {}", path.string()));
  }

  std::memcpy(address.sun_path, native.c_str(), native.size() + 1);
  if (::connect(fd,
                reinterpret_cast<const sockaddr*>(&address),
                static_cast<socklen_t>(sizeof(address))) != 0) {
    const auto error = std::error_code(errno, std::generic_category()).message();
    ::close(fd);
    return std::unexpected(
        std::format("failed to connect to shared-memory sidecar {}: {}", path.string(), error));
  }

  return kj::AutoCloseFd(fd);
}

} // namespace

class SpawnedProcess {
  // Spawn-local mode owns one child server process. The destructor enforces a
  // bounded shutdown so transport errors do not leak background servers.
public:
  explicit SpawnedProcess(pid_t pid) : pid_(pid) {}

  SpawnedProcess(const SpawnedProcess&) = delete;
  SpawnedProcess& operator=(const SpawnedProcess&) = delete;
  SpawnedProcess(SpawnedProcess&& other) noexcept : pid_(std::exchange(other.pid_, -1)) {}
  SpawnedProcess& operator=(SpawnedProcess&& other) noexcept {
    if (this != &other) {
      pid_ = std::exchange(other.pid_, -1);
    }
    return *this;
  }

  ~SpawnedProcess() {
    if (pid_ <= 0) {
      return;
    }

    int status = 0;
    if (::waitpid(pid_, &status, WNOHANG) != pid_) {
      ::kill(pid_, SIGTERM);
      const auto deadline = Clock::now() + std::chrono::seconds(2);
      while (Clock::now() < deadline) {
        if (::waitpid(pid_, &status, WNOHANG) == pid_) {
          pid_ = -1;
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, &status, 0);
    }

    pid_ = -1;
  }

private:
  pid_t pid_ = -1;
};

class PathCleanupGuard {
  // Removes a filesystem path before use and again on teardown. This keeps
  // spawn-local Unix sockets and shared-memory sidecar paths repeatable across
  // manual smoke runs.
public:
  explicit PathCleanupGuard(std::filesystem::path path) : path_(std::move(path)) {
    cleanup();
  }

  PathCleanupGuard(const PathCleanupGuard&) = delete;
  PathCleanupGuard& operator=(const PathCleanupGuard&) = delete;
  PathCleanupGuard(PathCleanupGuard&&) noexcept = default;
  PathCleanupGuard& operator=(PathCleanupGuard&&) noexcept = default;

  ~PathCleanupGuard() {
    cleanup();
  }

private:
  void cleanup() const {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  std::filesystem::path path_;
};

class ShmBenchmarkTransport {
  // Owns one shared-memory mapping and one parent-side wake broker for the
  // active benchmark run. Worker attachments borrow slot-local MessageStreams
  // from this context while it keeps the region and control channel alive.
public:
  static std::expected<std::unique_ptr<ShmBenchmarkTransport>, std::string>
  attach(std::string_view logical_name, std::size_t active_slot_count) {
    auto region = ShmTransportRegion::open(logical_name);
    if (!region) {
      return std::unexpected(region.error());
    }
    if (active_slot_count == 0) {
      return std::unexpected("shared-memory benchmark requires at least one active slot");
    }
    if (active_slot_count > region->options().slot_count) {
      return std::unexpected(std::format(
          "shared-memory benchmark requested {} slots but the server only provisioned {}",
          active_slot_count,
          region->options().slot_count));
    }

    auto control_fd = connect_sidecar_socket(derived_shm_sidecar_path(logical_name));
    if (!control_fd) {
      return std::unexpected(control_fd.error());
    }

    auto transport = std::unique_ptr<ShmBenchmarkTransport>(
        new ShmBenchmarkTransport(kj::mv(*region), active_slot_count));
    transport->wake_listeners_.reserve(active_slot_count);
    transport->wake_listener_ptrs_.reserve(active_slot_count);
    for (std::size_t slot_index = 0; slot_index < active_slot_count; ++slot_index) {
      transport->wake_listeners_.push_back(std::make_unique<CrossThreadWakeListener>());
      transport->wake_listener_ptrs_.push_back(transport->wake_listeners_.back().get());
    }

    transport->broker_ = std::make_unique<ParentWakeBroker>(
        control_fd->release(), std::span<CrossThreadWakeListener*>(transport->wake_listener_ptrs_));
    if (auto started = transport->broker_->start(); !started) {
      return std::unexpected(started.error());
    }
    if (auto attached = transport->broker_->send_attach(active_slot_count); !attached) {
      return std::unexpected(attached.error());
    }
    for (std::size_t slot_index = 0; slot_index < active_slot_count; ++slot_index) {
      if (auto sent = transport->broker_->send_init(slot_index); !sent) {
        return std::unexpected(sent.error());
      }
    }

    return transport;
  }

  ShmBenchmarkTransport(const ShmBenchmarkTransport&) = delete;
  ShmBenchmarkTransport& operator=(const ShmBenchmarkTransport&) = delete;
  ShmBenchmarkTransport(ShmBenchmarkTransport&&) = delete;
  ShmBenchmarkTransport& operator=(ShmBenchmarkTransport&&) = delete;

  ~ShmBenchmarkTransport() {
    if (broker_) {
      broker_->stop();
    }
  }

  [[nodiscard]] kj::Own<capnp::MessageStream> make_message_stream(std::size_t slot_index) {
    KJ_REQUIRE(broker_ != nullptr, "shared-memory benchmark transport is missing its wake broker");
    KJ_REQUIRE(slot_index < active_slot_count_, "shared-memory slot index is out of range");
    return kj::heap<ShmMessageStream>(slot_index,
                                      region_.benchmark_endpoint(slot_index),
                                      *wake_listeners_.at(slot_index),
                                      *broker_);
  }

  [[nodiscard]] std::optional<std::string> background_error() const {
    return broker_ ? broker_->background_error() : std::nullopt;
  }

private:
  ShmBenchmarkTransport(ShmTransportRegion&& region, std::size_t active_slot_count)
      : region_(kj::mv(region)), active_slot_count_(active_slot_count) {}

  ShmTransportRegion region_;
  std::size_t active_slot_count_ = 0;
  std::vector<std::unique_ptr<CrossThreadWakeListener>> wake_listeners_;
  std::vector<CrossThreadWakeListener*> wake_listener_ptrs_;
  std::unique_ptr<ParentWakeBroker> broker_;
};

WorkerAttachment::WorkerAttachment(kj::AutoCloseFd&& preconnected_fd)
    : kind_(Kind::preconnected_stream), preconnected_fd_(kj::mv(preconnected_fd)) {}

WorkerAttachment::WorkerAttachment(ShmBenchmarkTransport* shm_transport, std::size_t slot_index)
    : kind_(Kind::shared_memory), shm_transport_(shm_transport), shm_slot_index_(slot_index) {}

std::expected<RpcChannel, std::string>
WorkerAttachment::open_channel(const TransportUri& uri, kj::AsyncIoContext& io_context) && {
  switch (kind_) {
  case Kind::network:
    if (!uri.uses_kj_network()) {
      return std::unexpected(std::format(
          "transport {} requires transport-specific worker attachments", uri.to_string()));
    }
    return RpcChannel(connect_network_stream(uri, io_context));
  case Kind::preconnected_stream:
    if (preconnected_fd_.get() < 0) {
      return std::unexpected("pipe://socketpair worker attachment is missing its preconnected fd");
    }
    return RpcChannel(io_context.lowLevelProvider->wrapSocketFd(kj::mv(preconnected_fd_)));
  case Kind::shared_memory:
    if (shm_transport_ == nullptr) {
      return std::unexpected("shared-memory worker attachment is missing its transport context");
    }
    return RpcChannel(shm_transport_->make_message_stream(shm_slot_index_));
  }

  return std::unexpected("unsupported worker attachment kind");
}

std::expected<PreparedBenchmarkTransport, std::string>
PreparedBenchmarkTransport::prepare(const BenchmarkTransportConfig& config) {
  PreparedBenchmarkTransport prepared;
  prepared.resolved_uri_ =
      config.mode == BenchMode::connect ? *config.connect_uri : config.listen_uri;
  prepared.worker_attachments_.resize(config.client_threads);

  std::optional<SocketpairWorkers> socketpair_workers;
  if (config.mode == BenchMode::spawn_local) {
    auto ready_pipe = create_ready_pipe();
    if (!ready_pipe) {
      return std::unexpected(ready_pipe.error());
    }

    std::vector<std::string> args{
        std::format("--listen-uri={}", config.listen_uri.to_string()),
        std::format("--internal-ready-fd={}", ready_pipe->write_end.get()),
    };
    if (config.quiet_server) {
      args.emplace_back("--quiet");
    }

    switch (config.listen_uri.kind) {
    case TransportKind::tcp:
      break;
    case TransportKind::unix_socket:
      prepared.cleanup_guard_ =
          std::make_unique<PathCleanupGuard>(std::filesystem::path(config.listen_uri.location));
      break;
    case TransportKind::pipe_socketpair: {
      auto workers = create_socketpair_workers(config.client_threads);
      if (!workers) {
        return std::unexpected(workers.error());
      }
      socketpair_workers = std::move(*workers);

      std::string joined;
      for (std::size_t index = 0; index < socketpair_workers->server_fds.size(); ++index) {
        if (index > 0) {
          joined.push_back(',');
        }
        joined += std::to_string(socketpair_workers->server_fds[index].get());
      }
      args.push_back(std::format("--internal-preconnected-fds={}", joined));
      break;
    }
    case TransportKind::shared_memory:
      args.push_back(std::format("--shm-slot-count={}", config.client_threads));
      break;
    case TransportKind::unspecified:
      return std::unexpected("listen URI is not configured");
    }

    auto pid = spawn_process(std::filesystem::absolute(config.server_binary), args);
    if (!pid) {
      return std::unexpected(pid.error());
    }
    prepared.spawned_server_ = std::make_unique<SpawnedProcess>(*pid);
    ready_pipe->write_end = nullptr;

    if (auto ready = wait_for_ready_pipe(ready_pipe->read_end.get(),
                                         std::chrono::milliseconds(config.startup_timeout_ms));
        !ready) {
      return std::unexpected(ready.error());
    }

    if (socketpair_workers) {
      for (std::size_t index = 0; index < config.client_threads; ++index) {
        prepared.worker_attachments_[index] =
            WorkerAttachment(kj::mv(socketpair_workers->client_fds[index]));
      }
    }
  }

  if (prepared.resolved_uri_.kind == TransportKind::shared_memory) {
    auto shm_transport =
        ShmBenchmarkTransport::attach(prepared.resolved_uri_.location, config.client_threads);
    if (!shm_transport) {
      return std::unexpected(shm_transport.error());
    }
    prepared.shm_transport_ = std::move(*shm_transport);
    for (std::size_t index = 0; index < config.client_threads; ++index) {
      prepared.worker_attachments_[index] = WorkerAttachment(prepared.shm_transport_.get(), index);
    }
  }

  return prepared;
}

PreparedBenchmarkTransport::PreparedBenchmarkTransport() = default;
PreparedBenchmarkTransport::~PreparedBenchmarkTransport() = default;

PreparedBenchmarkTransport::PreparedBenchmarkTransport(
    PreparedBenchmarkTransport&& other) noexcept = default;

PreparedBenchmarkTransport&
PreparedBenchmarkTransport::operator=(PreparedBenchmarkTransport&& other) noexcept = default;

TransportUri PreparedBenchmarkTransport::resolved_uri() const {
  return resolved_uri_;
}

std::optional<std::string> PreparedBenchmarkTransport::background_error() const {
  return shm_transport_ ? shm_transport_->background_error() : std::nullopt;
}

std::vector<WorkerAttachment> PreparedBenchmarkTransport::take_worker_attachments() {
  return std::move(worker_attachments_);
}

} // namespace rpcbench
