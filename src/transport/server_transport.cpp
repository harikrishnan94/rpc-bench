// Server-side transport runtime. This implementation keeps listener setup and
// stream session ownership inside the transport layer so the application only
// needs to supply validated config and the bootstrap capability.

#include "transport/server_transport.hpp"

#include "transport/rpc_session.hpp"

#include <csignal>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <future>
#include <kj/async-io.h>
#include <kj/async-unix.h>
#include <kj/debug.h>
#include <kj/string.h>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace rpcbench {
namespace {

class SessionTasks final : public kj::TaskSet::ErrorHandler {
  // Owns background RPC sessions for one transport runtime. Each accepted
  // stream becomes one task so the listener can keep accepting while existing
  // sessions continue to make progress.
public:
  explicit SessionTasks(ServerBootstrapFactory bootstrap_factory)
      : bootstrap_(bootstrap_factory()), tasks_(*this) {}

  void add_stream(kj::Own<kj::AsyncIoStream>&& stream) {
    auto session = kj::heap<ServerRpcSession>(bootstrap_, kj::mv(stream));
    tasks_.add(session->run().catch_([](kj::Exception&&) {}).attach(kj::mv(session)));
  }

  void taskFailed(kj::Exception&& exception) override {
    std::println(stderr, "error: background server task failed: {}", kj::str(exception).cStr());
  }

private:
  capnp::Capability::Client bootstrap_;
  kj::TaskSet tasks_;
};

[[nodiscard]] kj::AutoCloseFd duplicate_stream_fd(const kj::AsyncIoStream& stream) {
  // Cross-thread handoff happens by duplicating the accepted socket so the
  // acceptor can release its KJ wrapper immediately while the worker thread
  // receives independent ownership of the same connection.
  KJ_IF_MAYBE (fd, stream.getFd()) {
    int duplicate = ::fcntl(*fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
      duplicate = ::dup(*fd);
    }
    if (duplicate < 0) {
      throw KJ_EXCEPTION(FAILED, "failed to duplicate accepted connection fd");
    }
    return kj::AutoCloseFd(duplicate);
  }

  throw KJ_EXCEPTION(FAILED, "accepted network stream does not expose a file descriptor");
}

struct WorkerStartup {
  // Cross-thread startup handoff from the worker thread back to the acceptor.
  // The acceptor uses the Executor for future dispatch and the fulfiller to
  // request worker shutdown during teardown.
  kj::Own<const kj::Executor> executor;
  kj::Own<kj::CrossThreadPromiseFulfiller<void>> shutdown_fulfiller;
};

class NetworkWorker final {
  // Owns one serving thread with its own KJ event loop. The acceptor uses the
  // worker's Executor to schedule fd wrapping on the correct event loop so each
  // accepted session stays on the worker that first receives it.
public:
  NetworkWorker(std::size_t worker_index,
                ServerBootstrapFactory bootstrap_factory,
                kj::CrossThreadPromiseFulfiller<void>& fatal_fulfiller)
      : worker_index_(worker_index), bootstrap_factory_(bootstrap_factory),
        fatal_fulfiller_(fatal_fulfiller) {}

  NetworkWorker(const NetworkWorker&) = delete;
  NetworkWorker& operator=(const NetworkWorker&) = delete;
  NetworkWorker(NetworkWorker&&) = delete;
  NetworkWorker& operator=(NetworkWorker&&) = delete;

  ~NetworkWorker() {
    request_shutdown();
    join();
  }

  void start() {
    std::promise<std::expected<WorkerStartup, std::string>> startup_promise;
    auto startup_future = startup_promise.get_future();
    thread_ = std::thread([this, startup_promise = std::move(startup_promise)]() mutable {
      thread_main(startup_promise);
    });

    auto startup = startup_future.get();
    if (!startup) {
      join();
      throw KJ_EXCEPTION(FAILED, startup.error().c_str());
    }

    executor_ = kj::mv(startup->executor);
    shutdown_fulfiller_ = kj::mv(startup->shutdown_fulfiller);
  }

  [[nodiscard]] kj::Promise<void> dispatch(kj::AutoCloseFd&& fd) {
    KJ_IF_MAYBE (executor, executor_) {
      return (*executor)->executeAsync(
          [worker = this, fd = kj::mv(fd)]() mutable { worker->accept_connection(kj::mv(fd)); });
    }

    throw KJ_EXCEPTION(FAILED, "network worker must be started before dispatch");
  }

  void request_shutdown() {
    KJ_IF_MAYBE (fulfiller, shutdown_fulfiller_) {
      (*fulfiller)->fulfill();
      shutdown_fulfiller_ = nullptr;
    }
  }

  void join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  void accept_connection(kj::AutoCloseFd&& fd) {
    KJ_REQUIRE(io_context_ != nullptr && sessions_ != nullptr,
               "network worker runtime must be live before dispatch");
    sessions_->add_stream(io_context_->lowLevelProvider->wrapSocketFd(kj::mv(fd)));
  }

  void thread_main(std::promise<std::expected<WorkerStartup, std::string>>& startup_promise) {
    bool startup_delivered = false;

    try {
      auto io_context = kj::setupAsyncIo();
      SessionTasks sessions(bootstrap_factory_);
      auto shutdown = kj::newPromiseAndCrossThreadFulfiller<void>();

      io_context_ = &io_context;
      sessions_ = &sessions;

      startup_promise.set_value(
          WorkerStartup{kj::getCurrentThreadExecutor().addRef(), kj::mv(shutdown.fulfiller)});
      startup_delivered = true;

      shutdown.promise.wait(io_context.waitScope);

      sessions_ = nullptr;
      io_context_ = nullptr;
    } catch (const kj::Exception& exception) {
      io_context_ = nullptr;
      sessions_ = nullptr;
      report_failure(
          startup_promise,
          startup_delivered,
          std::format("network worker {} failed: {}", worker_index_, kj::str(exception).cStr()));
    } catch (const std::exception& exception) {
      io_context_ = nullptr;
      sessions_ = nullptr;
      report_failure(startup_promise,
                     startup_delivered,
                     std::format("network worker {} failed: {}", worker_index_, exception.what()));
    } catch (...) {
      io_context_ = nullptr;
      sessions_ = nullptr;
      report_failure(
          startup_promise,
          startup_delivered,
          std::format("network worker {} failed with an unknown exception", worker_index_));
    }
  }

  void report_failure(std::promise<std::expected<WorkerStartup, std::string>>& startup_promise,
                      bool startup_delivered,
                      std::string message) {
    if (!startup_delivered) {
      startup_promise.set_value(
          std::expected<WorkerStartup, std::string>(std::unexpect, std::move(message)));
      return;
    }

    fatal_fulfiller_.reject(KJ_EXCEPTION(FAILED, message.c_str()));
  }

  std::size_t worker_index_;
  ServerBootstrapFactory bootstrap_factory_;
  kj::CrossThreadPromiseFulfiller<void>& fatal_fulfiller_;
  std::thread thread_;
  kj::Maybe<kj::Own<const kj::Executor>> executor_;
  kj::Maybe<kj::Own<kj::CrossThreadPromiseFulfiller<void>>> shutdown_fulfiller_;
  kj::AsyncIoContext* io_context_ = nullptr;
  SessionTasks* sessions_ = nullptr;
};

class NetworkWorkerSet final : public kj::TaskSet::ErrorHandler {
  // Coordinates the acceptor-to-worker handoff. The acceptor keeps the worker
  // selection policy and in-flight dispatch promises here so round-robin
  // scheduling remains deterministic and teardown can wait for queued handoffs.
public:
  NetworkWorkerSet(std::size_t worker_count,
                   ServerBootstrapFactory bootstrap_factory,
                   kj::CrossThreadPromiseFulfiller<void>& fatal_fulfiller)
      : dispatch_tasks_(*this), fatal_fulfiller_(fatal_fulfiller) {
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
      workers_.push_back(
          std::make_unique<NetworkWorker>(index, bootstrap_factory, fatal_fulfiller));
    }
  }

  void start_all() {
    for (const auto& worker : workers_) {
      worker->start();
    }
  }

  [[nodiscard]] kj::Promise<void> accept_loop(kj::ConnectionReceiver& listener) {
    while (true) {
      dispatch_stream(co_await listener.accept());
    }
  }

  [[nodiscard]] kj::Promise<void> on_dispatches_empty() {
    return dispatch_tasks_.onEmpty();
  }

  void taskFailed(kj::Exception&& exception) override {
    fatal_fulfiller_.reject(kj::mv(exception));
  }

private:
  void dispatch_stream(kj::Own<kj::AsyncIoStream>&& stream) {
    KJ_REQUIRE(!workers_.empty(), "network worker set must not be empty");

    auto fd = duplicate_stream_fd(*stream);
    auto& worker = *workers_[next_worker_];
    next_worker_ = (next_worker_ + 1) % workers_.size();

    dispatch_tasks_.add(worker.dispatch(kj::mv(fd)));
  }

  std::vector<std::unique_ptr<NetworkWorker>> workers_;
  kj::TaskSet dispatch_tasks_;
  kj::CrossThreadPromiseFulfiller<void>& fatal_fulfiller_;
  std::size_t next_worker_ = 0;
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

[[nodiscard]] kj::Promise<void>
run_network_server_transport_impl(kj::AsyncIoContext& io_context,
                                  const ServerTransportConfig& config,
                                  ServerBootstrapFactory bootstrap_factory) {
  std::optional<SocketPathGuard> socket_guard;
  if (config.listen_uri.kind == TransportKind::unix_socket) {
    socket_guard.emplace(std::filesystem::path(config.listen_uri.location));
  }

  auto failure = kj::newPromiseAndCrossThreadFulfiller<void>();
  auto failure_promise = kj::mv(failure.promise);
  auto failure_fulfiller = kj::mv(failure.fulfiller);

  NetworkWorkerSet workers(config.server_threads, bootstrap_factory, *failure_fulfiller);
  workers.start_all();

  auto address_text = config.listen_uri.to_kj_address();
  if (!address_text) {
    throw KJ_EXCEPTION(FAILED, "invalid KJ network address", address_text.error().c_str());
  }

  auto address = co_await io_context.provider->getNetwork().parseAddress(address_text->c_str());
  auto listener = address->listen();

  if (!config.quiet) {
    std::println("rpc-bench-server listening on {}", config.listen_uri.to_string());
  }
  notify_parent_ready(config.ready_fd);

  std::exception_ptr failure_exception;
  try {
    auto stop = wait_for_shutdown_signal(io_context).exclusiveJoin(kj::mv(failure_promise));
    co_await kj::mv(stop).exclusiveJoin(workers.accept_loop(*listener));
  } catch (...) {
    failure_exception = std::current_exception();
  }

  co_await workers.on_dispatches_empty();

  if (failure_exception != nullptr) {
    std::rethrow_exception(failure_exception);
  }
  co_return;
}

[[nodiscard]] kj::Promise<void>
run_pipe_server_transport_impl(kj::AsyncIoContext& io_context,
                               const ServerTransportConfig& config,
                               ServerBootstrapFactory bootstrap_factory) {
  if (config.server_threads > 1) {
    throw KJ_EXCEPTION(FAILED, "pipe://socketpair only supports --server-threads=1");
  }
  if (config.preconnected_stream_fds.empty()) {
    throw KJ_EXCEPTION(FAILED, "pipe://socketpair requires inherited preconnected fds");
  }

  SessionTasks sessions(bootstrap_factory);
  for (const int fd : config.preconnected_stream_fds) {
    sessions.add_stream(io_context.lowLevelProvider->wrapSocketFd(kj::AutoCloseFd(fd)));
  }

  if (!config.quiet) {
    std::println("rpc-bench-server listening on {}", config.listen_uri.to_string());
  }
  notify_parent_ready(config.ready_fd);

  co_await wait_for_shutdown_signal(io_context);
}

[[nodiscard]] kj::Promise<void>
run_server_transport_impl(kj::AsyncIoContext& io_context,
                          const ServerTransportConfig& config,
                          ServerBootstrapFactory bootstrap_factory) {
  switch (config.listen_uri.kind) {
  case TransportKind::tcp:
  case TransportKind::unix_socket:
    co_await run_network_server_transport_impl(io_context, config, bootstrap_factory);
    co_return;
  case TransportKind::pipe_socketpair:
    co_await run_pipe_server_transport_impl(io_context, config, bootstrap_factory);
    co_return;
  case TransportKind::unspecified:
    throw KJ_EXCEPTION(FAILED, "listen URI is not configured");
  }
}

} // namespace

int run_server_transport(const ServerTransportConfig& config,
                         ServerBootstrapFactory bootstrap_factory) {
  kj::UnixEventPort::captureSignal(SIGINT);
  kj::UnixEventPort::captureSignal(SIGTERM);

  auto io_context = kj::setupAsyncIo();
  run_server_transport_impl(io_context, config, bootstrap_factory).wait(io_context.waitScope);
  return 0;
}

} // namespace rpcbench
