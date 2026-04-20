// Server-side transport runtime. This implementation keeps listener setup,
// shared-memory rendezvous, and per-connection session ownership inside the
// transport layer so the CLI-facing server frontend only supplies config and
// the bootstrap capability.

#include "transport/server_transport.hpp"

#include "transport/rpc_session.hpp"
#include "transport/shm_transport.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <format>
#include <kj/async-io.h>
#include <kj/async-unix.h>
#include <kj/debug.h>
#include <kj/string.h>
#include <memory>
#include <print>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace rpcbench {
namespace {

class SessionTasks final : public kj::TaskSet::ErrorHandler {
  // Owns background RPC sessions for one transport runtime. Each session gets
  // its own task so accepted streams and MessageStream-backed slots can make
  // progress independently while the transport code continues to accept or
  // wait for shutdown.
public:
  explicit SessionTasks(capnp::Capability::Client bootstrap)
      : bootstrap_(kj::mv(bootstrap)), tasks_(*this) {}

  void add_channel(RpcChannel&& channel) {
    auto session = kj::heap<ServerRpcSession>(bootstrap_, kj::mv(channel));
    tasks_.add(session->run().catch_([](kj::Exception&&) {}).attach(kj::mv(session)));
  }

  [[nodiscard]] kj::Promise<void> accept_loop(kj::ConnectionReceiver& listener) {
    return listener.accept().then(
        [this, &listener](kj::Own<kj::AsyncIoStream>&& connection) mutable {
          add_channel(RpcChannel(kj::mv(connection)));
          return accept_loop(listener);
        });
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

class ShmRegionCleanupGuard {
  // Unlinks the named shared-memory object on shutdown so standalone shm
  // servers do not leave stale transport names behind after exit.
public:
  explicit ShmRegionCleanupGuard(ShmTransportRegion& region) : region_(&region) {}

  ShmRegionCleanupGuard(const ShmRegionCleanupGuard&) = delete;
  ShmRegionCleanupGuard& operator=(const ShmRegionCleanupGuard&) = delete;
  ShmRegionCleanupGuard(ShmRegionCleanupGuard&&) = delete;
  ShmRegionCleanupGuard& operator=(ShmRegionCleanupGuard&&) = delete;

  ~ShmRegionCleanupGuard() {
    if (region_ == nullptr) {
      return;
    }

    if (auto unlinked = region_->unlink(); !unlinked) {
      std::println(stderr, "error: failed to unlink shared-memory region: {}", unlinked.error());
    }
  }

private:
  ShmTransportRegion* region_ = nullptr;
};

class ShmServerBenchmarkSession {
  // Owns one benchmark attachment over the shared-memory sidecar connection.
  // The dispatcher learns how many slots are active for this benchmark run,
  // then the session spins up one MessageStream-backed RPC stack per active
  // slot until the benchmark disconnects.
public:
  ShmServerBenchmarkSession(capnp::Capability::Client& bootstrap,
                            ShmTransportRegion& region,
                            kj::Own<kj::AsyncIoStream>&& control_connection,
                            std::size_t slot_count)
      : region_(region), control_connection_(kj::mv(control_connection)), sessions_(bootstrap) {
    const auto fd = control_connection_->getFd();
    KJ_REQUIRE(fd != nullptr, "shared-memory control connection must expose a Unix fd");
    int control_write_fd = -1;
    KJ_IF_MAYBE (raw_fd, fd) {
      control_write_fd = ::dup(*raw_fd);
    }
    if (control_write_fd < 0) {
      const auto error_message = std::error_code(errno, std::generic_category()).message();
      throw KJ_EXCEPTION(
          FAILED, "dup() failed for shared-memory control socket", error_message.c_str());
    }
    control_writer_.emplace(control_write_fd);

    wake_listeners_.reserve(slot_count);
    wake_listener_ptrs_.reserve(slot_count);
    for (std::size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
      wake_listeners_.push_back(std::make_unique<SameThreadWakeListener>());
      wake_listener_ptrs_.push_back(wake_listeners_.back().get());
    }

    dispatcher_.emplace(*control_connection_,
                        std::span<SameThreadWakeListener*>(wake_listener_ptrs_));
  }

  [[nodiscard]] kj::Promise<void> run() {
    auto dispatcher_task = dispatcher_->run().fork();
    return dispatcher_->initialized().then(
        [this, dispatcher_task = kj::mv(dispatcher_task)]() mutable {
          const auto active_slots = dispatcher_->active_slot_count();
          for (std::size_t slot_index = 0; slot_index < active_slots; ++slot_index) {
            sessions_.add_channel(
                RpcChannel(kj::heap<ShmMessageStream>(slot_index,
                                                      region_.server_endpoint(slot_index),
                                                      *wake_listeners_[slot_index],
                                                      *control_writer_)));
          }
          return dispatcher_task.addBranch();
        });
  }

private:
  ShmTransportRegion& region_;
  kj::Own<kj::AsyncIoStream> control_connection_;
  std::optional<HybridControlSocket> control_writer_;
  std::vector<std::unique_ptr<SameThreadWakeListener>> wake_listeners_;
  std::vector<SameThreadWakeListener*> wake_listener_ptrs_;
  std::optional<ServerControlDispatcher> dispatcher_;
  SessionTasks sessions_;
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
    const auto error_message = std::error_code(errno, std::generic_category()).message();
    std::println(stderr, "error: failed to notify benchmark parent: {}", error_message);
  }
  ::close(*ready_fd);
}

[[nodiscard]] kj::Promise<void> shm_accept_loop(kj::ConnectionReceiver& listener,
                                                capnp::Capability::Client bootstrap,
                                                ShmTransportRegion& region,
                                                std::size_t slot_count) {
  return listener.accept().then(
      [&listener, bootstrap = kj::mv(bootstrap), &region, slot_count](
          kj::Own<kj::AsyncIoStream>&& control_connection) mutable -> kj::Promise<void> {
        auto next_bootstrap = capnp::Capability::Client(bootstrap);
        auto session = kj::heap<ShmServerBenchmarkSession>(
            bootstrap, region, kj::mv(control_connection), slot_count);
        return session->run()
            .catch_([](kj::Exception&& exception) {
              if (exception.getType() != kj::Exception::Type::DISCONNECTED) {
                std::println(stderr,
                             "error: shared-memory benchmark session failed: {}",
                             kj::str(exception).cStr());
              }
            })
            .then([&listener,
                   bootstrap = kj::mv(next_bootstrap),
                   &region,
                   slot_count,
                   session = kj::mv(session)]() mutable {
              return shm_accept_loop(listener, kj::mv(bootstrap), region, slot_count);
            })
            .attach(kj::mv(session));
      });
}

} // namespace

int run_server_transport(const ServerTransportConfig& config, capnp::Capability::Client bootstrap) {
  kj::UnixEventPort::captureSignal(SIGINT);
  kj::UnixEventPort::captureSignal(SIGTERM);

  auto io_context = kj::setupAsyncIo();

  switch (config.listen_uri.kind) {
  case TransportKind::tcp:
  case TransportKind::unix_socket: {
    auto address_text = config.listen_uri.to_kj_address();
    if (!address_text) {
      throw KJ_EXCEPTION(FAILED, "invalid KJ network address", address_text.error().c_str());
    }

    auto address = io_context.provider->getNetwork()
                       .parseAddress(address_text->c_str())
                       .wait(io_context.waitScope);
    auto listener = address->listen();
    SessionTasks sessions(bootstrap);

    if (!config.quiet) {
      std::println("rpc-bench-server listening on {}", config.listen_uri.to_string());
    }
    notify_parent_ready(config.ready_fd);

    wait_for_shutdown_signal(io_context)
        .exclusiveJoin(sessions.accept_loop(*listener))
        .wait(io_context.waitScope);
    return 0;
  }
  case TransportKind::pipe_socketpair: {
    SessionTasks sessions(bootstrap);
    for (const int fd : config.preconnected_stream_fds) {
      auto stream = io_context.lowLevelProvider->wrapSocketFd(kj::AutoCloseFd(fd));
      sessions.add_channel(RpcChannel(kj::mv(stream)));
    }

    if (!config.quiet) {
      std::println("rpc-bench-server listening on {}", config.listen_uri.to_string());
    }
    notify_parent_ready(config.ready_fd);

    wait_for_shutdown_signal(io_context).wait(io_context.waitScope);
    return 0;
  }
  case TransportKind::shared_memory: {
    if (config.shm_slot_count == 0) {
      throw KJ_EXCEPTION(FAILED, "shared-memory transport requires --shm-slot-count");
    }

    auto region = ShmTransportRegion::create(config.listen_uri.location,
                                             default_shm_transport_options(config.shm_slot_count));
    if (!region) {
      throw KJ_EXCEPTION(
          FAILED, "failed to create shared-memory transport region", region.error().c_str());
    }
    ShmRegionCleanupGuard region_guard(*region);

    const auto control_path = derived_shm_sidecar_path(config.listen_uri.location);
    SocketPathGuard control_guard(control_path);
    TransportUri control_uri{
        .kind = TransportKind::unix_socket,
        .location = control_path.string(),
    };
    auto control_address_text = control_uri.to_kj_address();
    if (!control_address_text) {
      throw KJ_EXCEPTION(
          FAILED, "invalid shared-memory control address", control_address_text.error().c_str());
    }

    auto control_address = io_context.provider->getNetwork()
                               .parseAddress(control_address_text->c_str())
                               .wait(io_context.waitScope);
    auto control_listener = control_address->listen();

    if (!config.quiet) {
      std::println("rpc-bench-server listening on {}", config.listen_uri.to_string());
    }
    notify_parent_ready(config.ready_fd);

    wait_for_shutdown_signal(io_context)
        .exclusiveJoin(
            shm_accept_loop(*control_listener, bootstrap, *region, config.shm_slot_count))
        .wait(io_context.waitScope);
    return 0;
  }
  case TransportKind::unspecified:
    KJ_FAIL_REQUIRE("listen URI is not configured");
  }

  KJ_UNREACHABLE;
}

} // namespace rpcbench
