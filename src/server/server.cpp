// CLI-facing server frontend for the CRC32 benchmark. This layer keeps only
// argument parsing and the hash-service bootstrap while delegating transport
// lifecycle to `src/transport/`.

#include "server/server.hpp"

#include "protocol/crc32.hpp"
#include "protocol/hash_service.capnp.h"
#include "protocol/service.hpp"
#include "transport/server_transport.hpp"

#include <capnp/rpc-twoparty.h>
#include <format>
#include <kj/debug.h>
#include <memory>
#include <print>
#include <span>
#include <string>
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

    if (const auto value = get_value(arg, "--shm-slot-count=")) {
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
    if (!config.preconnected_stream_fds.empty()) {
      return std::unexpected("internal preconnected fds are only valid with pipe://socketpair");
    }
    if (config.shm_slot_count != 0) {
      return std::unexpected("--shm-slot-count is only valid with shm://NAME");
    }
    break;
  case TransportKind::pipe_socketpair:
    if (config.preconnected_stream_fds.empty()) {
      return std::unexpected(
          "pipe://socketpair requires internal preconnected fds from spawn-local mode");
    }
    if (config.shm_slot_count != 0) {
      return std::unexpected("--shm-slot-count is not valid with pipe://socketpair");
    }
    break;
  case TransportKind::shared_memory:
    if (!config.preconnected_stream_fds.empty()) {
      return std::unexpected("shared-memory transport does not use internal preconnected fds");
    }
    if (config.shm_slot_count == 0) {
      return std::unexpected("shm://NAME requires --shm-slot-count=N");
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
                     "  --listen-uri=URI     Listen target. Default: tcp://127.0.0.1:7000\n"
                     "  --shm-slot-count=N   Required slot capacity for shm://NAME\n"
                     "  --quiet              Suppress the startup banner\n"
                     "  --help               Show this message\n",
                     program_name);
}

ServerApp::ServerApp(ServerConfig config) : config_(std::move(config)) {}

int ServerApp::run() {
  ServerTransportConfig transport_config{
      .listen_uri = config_.listen_uri,
      .quiet = config_.quiet,
      .ready_fd = config_.ready_fd,
      .preconnected_stream_fds = config_.preconnected_stream_fds,
      .shm_slot_count = config_.shm_slot_count,
  };
  return run_server_transport(transport_config, make_bootstrap_service());
}

} // namespace rpcbench
