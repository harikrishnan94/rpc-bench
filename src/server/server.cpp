// Single-threaded Cap'n Proto RPC server for the CRC32 benchmark. The
// implementation keeps one KJ event loop and one bootstrap capability so the
// server lifecycle stays small, explicit, and easy to reproduce.

#include "server/server.hpp"

#include "protocol/crc32.hpp"
#include "protocol/hash_service.capnp.h"

#include <capnp/rpc-twoparty.h>
#include <csignal>
#include <format>
#include <kj/async-io.h>
#include <kj/async-unix.h>
#include <kj/debug.h>
#include <kj/string.h>
#include <limits>
#include <print>
#include <span>
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

// Shutdown is driven by UnixEventPort signal promises so the listener can be
// canceled from within the same KJ event loop rather than by a helper thread.
kj::Promise<void> wait_for_shutdown_signal(kj::AsyncIoContext& io_context) {
  auto sigint = io_context.unixEventPort.onSignal(SIGINT).then([](siginfo_t&&) {});
  auto sigterm = io_context.unixEventPort.onSignal(SIGTERM).then([](siginfo_t&&) {});
  return sigint.exclusiveJoin(kj::mv(sigterm));
}

[[nodiscard]] std::uint16_t listener_port(kj::ConnectionReceiver& listener) {
  const auto port = listener.getPort();
  KJ_REQUIRE(port <= std::numeric_limits<std::uint16_t>::max(), "listener port out of range", port);
  return static_cast<std::uint16_t>(port);
}

} // namespace

Endpoint ServerConfig::endpoint() const {
  return Endpoint{
      .host = listen_host,
      .port = port,
  };
}

std::expected<ServerConfig, std::string>
parse_server_config(std::span<const std::string_view> args) {
  ServerConfig config;

  for (const auto arg : args) {
    if (has_flag(arg, "--quiet")) {
      config.quiet = true;
      continue;
    }

    if (const auto value = get_value(arg, "--listen-host=")) {
      config.listen_host = std::string(*value);
      continue;
    }

    if (const auto value = get_value(arg, "--port=")) {
      auto parsed = parse_port(*value, "port");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.port = *parsed;
      continue;
    }

    return std::unexpected(std::format("unknown server argument '{}'", arg));
  }

  if (config.listen_host.empty()) {
    return std::unexpected("listen host must not be empty");
  }

  return config;
}

std::string server_usage(std::string_view program_name) {
  return std::format("Usage: {} [options]\n"
                     "\n"
                     "Options:\n"
                     "  --listen-host=HOST  Bind host or address. Default: 127.0.0.1\n"
                     "  --port=PORT         TCP port. Default: 7000\n"
                     "  --quiet             Suppress the startup banner\n"
                     "  --help              Show this message\n",
                     program_name);
}

ServerApp::ServerApp(ServerConfig config) : config_(std::move(config)) {}

int ServerApp::run() {
  kj::UnixEventPort::captureSignal(SIGINT);
  kj::UnixEventPort::captureSignal(SIGTERM);

  auto io_context = kj::setupAsyncIo();
  auto address = io_context.provider->getNetwork()
                     .parseAddress(config_.listen_host.c_str(), config_.port)
                     .wait(io_context.waitScope);
  auto listener = address->listen();

  if (!config_.quiet) {
    std::println("rpc-bench-server listening on {}",
                 Endpoint{
                     .host = config_.listen_host,
                     .port = listener_port(*listener),
                 }
                     .to_string());
  }

  capnp::TwoPartyServer server(kj::heap<HashServiceImpl>());
  wait_for_shutdown_signal(io_context)
      .exclusiveJoin(server.listen(*listener))
      .wait(io_context.waitScope);
  return 0;
}

} // namespace rpcbench
