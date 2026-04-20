// Server-side transport runtime. This implementation keeps listener setup and
// stream session ownership inside the transport layer so the application only
// needs to supply validated config and the bootstrap capability.

#include "transport/server_transport.hpp"

#include "transport/rpc_session.hpp"

#include <csignal>
#include <filesystem>
#include <kj/async-io.h>
#include <kj/async-unix.h>
#include <kj/debug.h>
#include <kj/string.h>
#include <memory>
#include <optional>
#include <print>
#include <unistd.h>
#include <utility>

namespace rpcbench {
namespace {

class SessionTasks final : public kj::TaskSet::ErrorHandler {
  // Owns background RPC sessions for one transport runtime. Each accepted
  // stream becomes one task so the listener can keep accepting while existing
  // sessions continue to make progress.
public:
  explicit SessionTasks(capnp::Capability::Client bootstrap)
      : bootstrap_(kj::mv(bootstrap)), tasks_(*this) {}

  void add_stream(kj::Own<kj::AsyncIoStream>&& stream) {
    auto session = kj::heap<ServerRpcSession>(bootstrap_, kj::mv(stream));
    tasks_.add(session->run().catch_([](kj::Exception&&) {}).attach(kj::mv(session)));
  }

  [[nodiscard]] kj::Promise<void> accept_loop(kj::ConnectionReceiver& listener) {
    while (true) {
      add_stream(co_await listener.accept());
    }
  }

  void taskFailed(kj::Exception&& exception) override {
    std::println(stderr, "error: background server task failed: {}", kj::str(exception).cStr());
  }

private:
  capnp::Capability::Client bootstrap_;
  kj::TaskSet tasks_;
};

class SocketPathGuard {
  // Removes a Unix socket path before use and again on teardown so repeated
  // smoke runs do not get stuck on stale filesystem entries.
public:
  explicit SocketPathGuard(std::filesystem::path path) : path_(std::move(path)) {
    cleanup();
  }

  SocketPathGuard(const SocketPathGuard&) = delete;
  SocketPathGuard& operator=(const SocketPathGuard&) = delete;
  SocketPathGuard(SocketPathGuard&&) noexcept = default;
  SocketPathGuard& operator=(SocketPathGuard&&) noexcept = default;

  ~SocketPathGuard() {
    cleanup();
  }

private:
  void cleanup() const {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  std::filesystem::path path_;
};

// Shutdown is driven by UnixEventPort signal promises so transports can be
// cancelled from within the same KJ event loop rather than by helper threads.
kj::Promise<void> wait_for_shutdown_signal(kj::AsyncIoContext& io_context) {
  auto sigint = io_context.unixEventPort.onSignal(SIGINT).then([](siginfo_t&&) {});
  auto sigterm = io_context.unixEventPort.onSignal(SIGTERM).then([](siginfo_t&&) {});
  return sigint.exclusiveJoin(kj::mv(sigterm));
}

void notify_parent_ready(const std::optional<int>& ready_fd) {
  if (!ready_fd) {
    return;
  }

  static constexpr std::byte kReadyByte{std::byte{1}};
  const auto* bytes = reinterpret_cast<const char*>(&kReadyByte);
  if (::write(*ready_fd, bytes, 1) < 0) {
    std::println(stderr, "error: failed to notify benchmark parent");
  }
  ::close(*ready_fd);
}

[[nodiscard]] kj::Promise<void> run_server_transport_impl(kj::AsyncIoContext& io_context,
                                                          const ServerTransportConfig& config,
                                                          capnp::Capability::Client bootstrap) {
  if (config.server_threads > 1) {
    throw KJ_EXCEPTION(FAILED,
                       "multi-threaded server is not implemented; --server-threads must be 1");
  }

  switch (config.listen_uri.kind) {
  case TransportKind::tcp:
  case TransportKind::unix_socket: {
    std::optional<SocketPathGuard> socket_guard;
    if (config.listen_uri.kind == TransportKind::unix_socket) {
      socket_guard.emplace(std::filesystem::path(config.listen_uri.location));
    }

    auto address_text = config.listen_uri.to_kj_address();
    if (!address_text) {
      throw KJ_EXCEPTION(FAILED, "invalid KJ network address", address_text.error().c_str());
    }

    auto address = co_await io_context.provider->getNetwork().parseAddress(address_text->c_str());
    auto listener = address->listen();
    SessionTasks sessions(kj::mv(bootstrap));

    if (!config.quiet) {
      std::println("rpc-bench-server listening on {}", config.listen_uri.to_string());
    }
    notify_parent_ready(config.ready_fd);

    co_await wait_for_shutdown_signal(io_context).exclusiveJoin(sessions.accept_loop(*listener));
    co_return;
  }
  case TransportKind::pipe_socketpair: {
    if (config.preconnected_stream_fds.empty()) {
      throw KJ_EXCEPTION(FAILED, "pipe://socketpair requires inherited preconnected fds");
    }

    SessionTasks sessions(kj::mv(bootstrap));
    for (const int fd : config.preconnected_stream_fds) {
      sessions.add_stream(io_context.lowLevelProvider->wrapSocketFd(kj::AutoCloseFd(fd)));
    }

    if (!config.quiet) {
      std::println("rpc-bench-server listening on {}", config.listen_uri.to_string());
    }
    notify_parent_ready(config.ready_fd);

    co_await wait_for_shutdown_signal(io_context);
    co_return;
  }
  case TransportKind::unspecified:
    throw KJ_EXCEPTION(FAILED, "listen URI is not configured");
  }
}

} // namespace

int run_server_transport(const ServerTransportConfig& config, capnp::Capability::Client bootstrap) {
  kj::UnixEventPort::captureSignal(SIGINT);
  kj::UnixEventPort::captureSignal(SIGTERM);

  auto io_context = kj::setupAsyncIo();
  run_server_transport_impl(io_context, config, kj::mv(bootstrap)).wait(io_context.waitScope);
  return 0;
}

} // namespace rpcbench
