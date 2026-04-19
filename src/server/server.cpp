// Single-threaded Cap'n Proto RPC server for the CRC32 benchmark. The
// implementation keeps one KJ event loop and one bootstrap capability so the
// server lifecycle stays small, explicit, and easy to reproduce.

#include "server/server.hpp"

#include "protocol/crc32.hpp"
#include "protocol/hash_service.capnp.h"
#include "protocol/shm_transport.hpp"

#include <capnp/rpc-twoparty.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <format>
#include <kj/async-io.h>
#include <kj/async-unix.h>
#include <kj/debug.h>
#include <kj/string.h>
#include <memory>
#include <print>
#include <span>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace rpcbench {
namespace {

[[nodiscard]] std::span<const std::byte> to_byte_span(capnp::Data::Reader payload) {
  return {
      reinterpret_cast<const std::byte*>(payload.begin()),
      payload.size(),
  };
}

class HashServiceImpl final : public HashService::Server {
  // Implements the benchmark's one unary RPC. The method keeps the 1 MiB
  // payload contract in the service layer so oversized direct RPC callers fail
  // cleanly even outside the benchmark CLI.
public:
  kj::Promise<void> hash(HashContext context) override {
    const auto payload = context.getParams().getPayload();
    KJ_REQUIRE(payload.size() <= kMaxPayloadSizeBytes,
               "payload exceeds maximum size",
               payload.size(),
               kMaxPayloadSizeBytes);

    context.getResults().setCrc32(compute_crc32(to_byte_span(payload)));
    co_return;
  }
};

// One accepted stream or custom message stream corresponds to one Cap'n Proto
// two-party session. This object owns the transport and the RPC stack together
// so the bootstrap capability outlives all in-flight RPC work on that session.
class ServerRpcSession {
public:
  ServerRpcSession(capnp::Capability::Client bootstrap, kj::Own<kj::AsyncIoStream>&& connection)
      : connection_(kj::mv(connection)), network_(*connection_, capnp::rpc::twoparty::Side::SERVER),
        rpc_system_(capnp::makeRpcServer(network_, kj::mv(bootstrap))) {}

  ServerRpcSession(capnp::Capability::Client bootstrap,
                   kj::Own<capnp::MessageStream>&& message_stream)
      : message_stream_(kj::mv(message_stream)),
        network_(*message_stream_, capnp::rpc::twoparty::Side::SERVER),
        rpc_system_(capnp::makeRpcServer(network_, kj::mv(bootstrap))) {}

  [[nodiscard]] kj::Promise<void> run() {
    return rpc_system_.run().exclusiveJoin(network_.onDisconnect());
  }

private:
  kj::Own<kj::AsyncIoStream> connection_;
  kj::Own<capnp::MessageStream> message_stream_;
  capnp::TwoPartyVatNetwork network_;
  capnp::RpcSystem<capnp::rpc::twoparty::VatId> rpc_system_;
};

// The server keeps per-connection work in a TaskSet so accepted sessions can
// progress independently while the main thread keeps its focus on accepting new
// streams or waiting for shutdown.
class SessionTasks final : public kj::TaskSet::ErrorHandler {
public:
  explicit SessionTasks(capnp::Capability::Client bootstrap)
      : bootstrap_(kj::mv(bootstrap)), tasks_(*this) {}

  void add_stream(kj::Own<kj::AsyncIoStream>&& connection) {
    auto session = kj::heap<ServerRpcSession>(bootstrap_, kj::mv(connection));
    tasks_.add(session->run().catch_([](kj::Exception&&) {}).attach(kj::mv(session)));
  }

  void add_message_stream(kj::Own<capnp::MessageStream>&& message_stream) {
    auto session = kj::heap<ServerRpcSession>(bootstrap_, kj::mv(message_stream));
    tasks_.add(session->run().catch_([](kj::Exception&&) {}).attach(kj::mv(session)));
  }

  [[nodiscard]] kj::Promise<void> accept_loop(kj::ConnectionReceiver& listener) {
    return listener.accept().then(
        [this, &listener](kj::Own<kj::AsyncIoStream>&& connection) mutable {
          add_stream(kj::mv(connection));
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

// Shutdown is driven by UnixEventPort signal promises so the listener can be
// canceled from within the same KJ event loop rather than by a helper thread.
kj::Promise<void> wait_for_shutdown_signal(kj::AsyncIoContext& io_context) {
  auto sigint = io_context.unixEventPort.onSignal(SIGINT).then([](siginfo_t&&) {});
  auto sigterm = io_context.unixEventPort.onSignal(SIGTERM).then([](siginfo_t&&) {});
  return sigint.exclusiveJoin(kj::mv(sigterm));
}

[[nodiscard]] std::expected<int, std::string> parse_fd_value(std::string_view text,
                                                             std::string_view name) {
  auto parsed = parse_integer<int>(text, name);
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  if (*parsed < 0) {
    return std::unexpected(std::format("{} must be zero or greater", name));
  }
  return *parsed;
}

[[nodiscard]] std::expected<std::vector<int>, std::string> parse_fd_list(std::string_view text,
                                                                         const char* value_name) {
  std::vector<int> fds;
  if (text.empty()) {
    return fds;
  }

  std::size_t begin = 0;
  while (begin <= text.size()) {
    const auto comma = text.find(',', begin);
    const auto token =
        comma == std::string_view::npos ? text.substr(begin) : text.substr(begin, comma - begin);
    auto parsed = parse_fd_value(token, value_name);
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    fds.push_back(*parsed);

    if (comma == std::string_view::npos) {
      break;
    }
    begin = comma + 1;
  }

  return fds;
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

[[nodiscard]] capnp::Capability::Client make_bootstrap_service() {
  return kj::heap<HashServiceImpl>();
}

} // namespace

std::expected<ServerConfig, std::string>
parse_server_config(std::span<const std::string_view> args) {
  ServerConfig config;

  for (const auto arg : args) {
    if (has_flag(arg, "--quiet")) {
      config.quiet = true;
      continue;
    }

    if (const auto value = get_value(arg, "--listen-uri=")) {
      auto parsed = parse_transport_uri(*value);
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.listen_uri = *parsed;
      continue;
    }

    if (const auto value = get_value(arg, "--internal-ready-fd=")) {
      auto parsed = parse_fd_value(*value, "ready fd");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.ready_fd = *parsed;
      continue;
    }

    if (const auto value = get_value(arg, "--internal-preconnected-fds=")) {
      auto parsed = parse_fd_list(*value, "preconnected fd");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.preconnected_stream_fds = std::move(*parsed);
      continue;
    }

    if (const auto value = get_value(arg, "--internal-shm-control-fd=")) {
      auto parsed = parse_fd_value(*value, "shared-memory control fd");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.shm_control_fd = *parsed;
      continue;
    }

    if (const auto value = get_value(arg, "--internal-shm-slot-count=")) {
      auto parsed = parse_integer<unsigned long long>(*value, "shared-memory slot count");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.shm_slot_count = static_cast<std::size_t>(*parsed);
      continue;
    }

    return std::unexpected(std::format("unknown server argument '{}'", arg));
  }

  if (config.listen_uri.kind == TransportKind::unspecified) {
    return std::unexpected("listen URI must not be empty");
  }

  switch (config.listen_uri.kind) {
  case TransportKind::tcp:
  case TransportKind::unix_socket:
    break;
  case TransportKind::pipe_socketpair:
    if (config.preconnected_stream_fds.empty()) {
      return std::unexpected(
          "pipe://socketpair requires internal preconnected fds from spawn-local mode");
    }
    break;
  case TransportKind::shared_memory:
    if (!config.shm_control_fd || config.shm_slot_count == 0) {
      return std::unexpected(
          "shm://NAME requires internal control-fd and slot-count from spawn-local mode");
    }
    break;
  case TransportKind::unspecified:
    return std::unexpected("listen URI must not be empty");
  }

  return config;
}

std::string server_usage(std::string_view program_name) {
  return std::format("Usage: {} [options]\n"
                     "\n"
                     "Options:\n"
                     "  --listen-uri=URI  Listen target. Default: tcp://127.0.0.1:7000\n"
                     "  --quiet           Suppress the startup banner\n"
                     "  --help            Show this message\n",
                     program_name);
}

ServerApp::ServerApp(ServerConfig config) : config_(std::move(config)) {}

int ServerApp::run() {
  kj::UnixEventPort::captureSignal(SIGINT);
  kj::UnixEventPort::captureSignal(SIGTERM);

  auto io_context = kj::setupAsyncIo();
  auto bootstrap = make_bootstrap_service();

  switch (config_.listen_uri.kind) {
  case TransportKind::tcp:
  case TransportKind::unix_socket: {
    auto address_text = config_.listen_uri.to_kj_address();
    if (!address_text) {
      throw KJ_EXCEPTION(FAILED, "invalid KJ network address", address_text.error().c_str());
    }

    auto address = io_context.provider->getNetwork()
                       .parseAddress(address_text->c_str())
                       .wait(io_context.waitScope);
    auto listener = address->listen();
    SessionTasks sessions(bootstrap);

    if (!config_.quiet) {
      std::println("rpc-bench-server listening on {}", config_.listen_uri.to_string());
    }
    notify_parent_ready(config_.ready_fd);

    wait_for_shutdown_signal(io_context)
        .exclusiveJoin(sessions.accept_loop(*listener))
        .wait(io_context.waitScope);
    return 0;
  }
  case TransportKind::pipe_socketpair: {
    SessionTasks sessions(bootstrap);
    for (const int fd : config_.preconnected_stream_fds) {
      auto stream = io_context.lowLevelProvider->wrapSocketFd(kj::AutoCloseFd(fd));
      sessions.add_stream(kj::mv(stream));
    }

    if (!config_.quiet) {
      std::println("rpc-bench-server listening on {}", config_.listen_uri.to_string());
    }
    notify_parent_ready(config_.ready_fd);

    wait_for_shutdown_signal(io_context).wait(io_context.waitScope);
    return 0;
  }
  case TransportKind::shared_memory: {
    if (!config_.shm_control_fd) {
      throw KJ_EXCEPTION(FAILED, "shared-memory control fd is not configured");
    }

    auto region = ShmTransportRegion::open(config_.listen_uri.location);
    if (!region) {
      throw KJ_EXCEPTION(
          FAILED, "failed to open shared-memory transport region", region.error().c_str());
    }

    const int control_write_fd = ::dup(*config_.shm_control_fd);
    if (control_write_fd < 0) {
      const auto error_message = std::error_code(errno, std::generic_category()).message();
      throw KJ_EXCEPTION(
          FAILED, "dup() failed for shared-memory control socket", error_message.c_str());
    }

    auto control_stream =
        io_context.lowLevelProvider->wrapSocketFd(kj::AutoCloseFd(*config_.shm_control_fd));
    HybridControlSocket control_writer(control_write_fd);

    std::vector<std::unique_ptr<SameThreadWakeListener>> wake_listeners;
    wake_listeners.reserve(config_.shm_slot_count);
    std::vector<SameThreadWakeListener*> wake_listener_ptrs;
    wake_listener_ptrs.reserve(config_.shm_slot_count);
    for (std::size_t slot_index = 0; slot_index < config_.shm_slot_count; ++slot_index) {
      wake_listeners.push_back(std::make_unique<SameThreadWakeListener>());
      wake_listener_ptrs.push_back(wake_listeners.back().get());
    }

    ServerControlDispatcher dispatcher(*control_stream,
                                       std::span<SameThreadWakeListener*>(wake_listener_ptrs));
    auto dispatcher_task = dispatcher.run().fork();

    SessionTasks sessions(bootstrap);
    for (std::size_t slot_index = 0; slot_index < config_.shm_slot_count; ++slot_index) {
      sessions.add_message_stream(kj::heap<ShmMessageStream>(slot_index,
                                                             region->server_endpoint(slot_index),
                                                             *wake_listeners[slot_index],
                                                             control_writer));
    }

    dispatcher.initialized().wait(io_context.waitScope);
    if (!config_.quiet) {
      std::println("rpc-bench-server listening on {}", config_.listen_uri.to_string());
    }
    notify_parent_ready(config_.ready_fd);

    wait_for_shutdown_signal(io_context)
        .exclusiveJoin(dispatcher_task.addBranch())
        .wait(io_context.waitScope);
    return 0;
  }
  case TransportKind::unspecified:
    KJ_FAIL_REQUIRE("listen URI is not configured");
  }

  KJ_UNREACHABLE;
}

} // namespace rpcbench
