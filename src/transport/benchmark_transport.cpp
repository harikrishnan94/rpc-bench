// Benchmark-side transport orchestration. This implementation keeps process
// management, URI-aware setup, and pipe socketpair provisioning out of the
// benchmark hot path so the runtime itself only sees async stream openers.

#include "transport/benchmark_transport.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <format>
#include <kj/async-unix.h>
#include <kj/debug.h>
#include <memory>
#include <poll.h>
#include <sys/socket.h>
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

struct SocketpairConnections {
  // pipe://socketpair mode allocates one parent-side and one child-side socket
  // per logical benchmark connection before the child server is exec'd.
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
  return std::move(pipe);
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

[[nodiscard]] std::expected<SocketpairConnections, std::string>
create_socketpair_connections(std::size_t count) {
  SocketpairConnections connections;
  connections.client_fds.reserve(count);
  connections.server_fds.reserve(count);

  for (std::size_t index = 0; index < count; ++index) {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      return std::unexpected("socketpair() failed while preparing pipe://socketpair connections");
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

    connections.server_fds.emplace_back(kj::mv(server_end));
    connections.client_fds.emplace_back(kj::mv(client_end));
  }

  return std::move(connections);
}

class NetworkConnectionOpener final : public ConnectionOpener {
  // Opens one TCP or Unix-domain stream for one logical benchmark connection.
public:
  explicit NetworkConnectionOpener(TransportUri uri) : uri_(std::move(uri)) {}

  [[nodiscard]] kj::Promise<kj::Own<kj::AsyncIoStream>>
  open(kj::AsyncIoContext& io_context) override {
    auto address_text = uri_.to_kj_address();
    if (!address_text) {
      KJ_FAIL_REQUIRE("invalid KJ network address", address_text.error().c_str());
    }

    auto address = co_await io_context.provider->getNetwork().parseAddress(address_text->c_str());
    co_return co_await address->connect();
  }

private:
  TransportUri uri_;
};

class PreconnectedConnectionOpener final : public ConnectionOpener {
  // Owns one inherited socketpair endpoint for one logical benchmark
  // connection. The opener is one-shot so the preconnected stream cannot be
  // reused accidentally.
public:
  explicit PreconnectedConnectionOpener(kj::AutoCloseFd&& fd) : fd_(kj::mv(fd)) {}
  ~PreconnectedConnectionOpener() noexcept override = default;

  [[nodiscard]] kj::Promise<kj::Own<kj::AsyncIoStream>>
  open(kj::AsyncIoContext& io_context) override {
    if (fd_.get() < 0) {
      KJ_FAIL_REQUIRE("pipe://socketpair opener is missing its preconnected fd");
    }

    co_return io_context.lowLevelProvider->wrapSocketFd(kj::mv(fd_));
  }

private:
  kj::AutoCloseFd fd_;
};

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
  // spawn-local Unix sockets repeatable across manual and automated runs.
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

std::expected<PreparedBenchmarkTransport, std::string>
PreparedBenchmarkTransport::prepare(const BenchmarkTransportConfig& config) {
  if (config.client_connections == 0) {
    return std::unexpected("client connection count must be greater than zero");
  }

  PreparedBenchmarkTransport prepared;
  const auto resolved_uri =
      config.mode == BenchMode::connect ? *config.connect_uri : config.listen_uri;
  prepared.resolved_uri_ = resolved_uri.to_string();
  prepared.connection_openers_.reserve(config.client_connections);

  std::optional<SocketpairConnections> socketpairs;
  if (config.mode == BenchMode::spawn_local) {
    auto ready_pipe = create_ready_pipe();
    if (!ready_pipe) {
      return std::unexpected(ready_pipe.error());
    }

    std::vector<std::string> args{
        std::format("--listen-uri={}", config.listen_uri.to_string()),
        std::format("--internal-ready-fd={}", ready_pipe->write_end.get()),
        std::format("--server-threads={}", config.server_threads),
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
      auto created = create_socketpair_connections(config.client_connections);
      if (!created) {
        return std::unexpected(created.error());
      }
      socketpairs = std::move(*created);

      std::string joined;
      for (std::size_t index = 0; index < socketpairs->server_fds.size(); ++index) {
        if (index > 0) {
          joined.push_back(',');
        }
        joined += std::to_string(socketpairs->server_fds[index].get());
      }
      args.push_back(std::format("--internal-preconnected-fds={}", joined));
      break;
    }
    case TransportKind::unspecified:
      return std::unexpected("listen URI is not configured");
    }

    auto pid = spawn_process(std::filesystem::absolute(config.server_binary), args);
    if (!pid) {
      return std::unexpected(pid.error());
    }
    prepared.spawned_server_ = std::make_unique<SpawnedProcess>(*pid);
    ready_pipe->write_end = kj::AutoCloseFd();

    if (auto ready = wait_for_ready_pipe(ready_pipe->read_end.get(),
                                         std::chrono::milliseconds(config.startup_timeout_ms));
        !ready) {
      return std::unexpected(ready.error());
    }
  }

  switch (resolved_uri.kind) {
  case TransportKind::tcp:
  case TransportKind::unix_socket:
    for (std::size_t index = 0; index < config.client_connections; ++index) {
      prepared.connection_openers_.push_back(kj::heap<NetworkConnectionOpener>(resolved_uri));
    }
    break;
  case TransportKind::pipe_socketpair:
    if (!socketpairs) {
      return std::unexpected("pipe://socketpair requires spawn-local socketpairs");
    }
    for (auto& fd : socketpairs->client_fds) {
      prepared.connection_openers_.push_back(kj::heap<PreconnectedConnectionOpener>(kj::mv(fd)));
    }
    break;
  case TransportKind::unspecified:
    return std::unexpected("transport URI is not configured");
  }

  return prepared;
}

PreparedBenchmarkTransport::PreparedBenchmarkTransport() = default;
PreparedBenchmarkTransport::~PreparedBenchmarkTransport() = default;

PreparedBenchmarkTransport::PreparedBenchmarkTransport(
    PreparedBenchmarkTransport&& other) noexcept = default;

PreparedBenchmarkTransport&
PreparedBenchmarkTransport::operator=(PreparedBenchmarkTransport&& other) noexcept = default;

std::string PreparedBenchmarkTransport::resolved_uri() const {
  return resolved_uri_;
}

std::vector<kj::Own<ConnectionOpener>> PreparedBenchmarkTransport::take_connection_openers() {
  return std::move(connection_openers_);
}

} // namespace rpcbench
