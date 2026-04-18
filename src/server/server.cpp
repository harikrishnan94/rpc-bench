// Single-threaded async TCP server for the CRC32 benchmark. The implementation
// keeps one event loop and one request/response coroutine per connection so the
// wire semantics stay simple and easy to reproduce.

#include "server/server.hpp"

#include "protocol/crc32.hpp"
#include "protocol/framing.hpp"

#include <array>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/signal_set.hpp>
#include <asio/socket_base.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <csignal>
#include <format>
#include <print>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace rpcbench {
namespace {

namespace asio_ns = asio;
using tcp = asio_ns::ip::tcp;

[[nodiscard]] tcp::endpoint resolve_bind_endpoint(asio_ns::io_context& io_context,
                                                  const ServerConfig& config) {
  std::error_code error;
  auto address = asio_ns::ip::make_address(config.listen_host, error);
  if (!error) {
    return tcp::endpoint(address, config.port);
  }

  tcp::resolver resolver(io_context);
  auto endpoints = resolver.resolve(
      config.listen_host, std::to_string(config.port), tcp::resolver::passive, error);
  if (error || endpoints.empty()) {
    throw std::runtime_error(
        std::format("could not resolve listen host '{}': {}", config.listen_host, error.message()));
  }
  return endpoints.begin()->endpoint();
}

// Closing an invalid request socket immediately is the intended oversize-frame
// policy, so the helper swallows close errors instead of surfacing a second,
// less useful failure path.
void close_socket(tcp::socket& socket) {
  std::error_code ignored;
  socket.shutdown(tcp::socket::shutdown_both, ignored);
  socket.close(ignored);
}

// Each connection is intentionally serial. The next frame is not read until the
// current CRC32 reply has been fully written back to the client.
asio_ns::awaitable<void> serve_connection(tcp::socket socket) {
  for (;;) {
    std::array<std::byte, kFrameHeaderBytes> header{};
    std::error_code error;
    co_await asio_ns::async_read(
        socket, asio_ns::buffer(header), asio_ns::redirect_error(asio_ns::use_awaitable, error));

    if (error == asio_ns::error::eof || error == asio_ns::error::connection_reset) {
      co_return;
    }
    if (error) {
      throw std::system_error(error);
    }

    const auto payload_size = decode_be32(header);
    if (payload_size > kMaxFrameSizeBytes) {
      close_socket(socket);
      co_return;
    }

    std::vector<std::byte> payload(payload_size);
    if (!payload.empty()) {
      co_await asio_ns::async_read(
          socket, asio_ns::buffer(payload), asio_ns::redirect_error(asio_ns::use_awaitable, error));
      if (error == asio_ns::error::eof || error == asio_ns::error::connection_reset) {
        co_return;
      }
      if (error) {
        throw std::system_error(error);
      }
    }

    const auto reply = encode_be32(compute_crc32(payload));
    co_await asio_ns::async_write(
        socket, asio_ns::buffer(reply), asio_ns::redirect_error(asio_ns::use_awaitable, error));
    if (error == asio_ns::error::eof || error == asio_ns::error::connection_reset) {
      co_return;
    }
    if (error) {
      throw std::system_error(error);
    }
  }
}

// The accept loop owns connection fan-out only. Per-connection failures stay
// isolated inside their detached coroutine so one bad client cannot stop the
// listener for everyone else.
asio_ns::awaitable<void> accept_loop(tcp::acceptor& acceptor) {
  const auto executor = co_await asio_ns::this_coro::executor;

  for (;;) {
    std::error_code error;
    tcp::socket socket =
        co_await acceptor.async_accept(asio_ns::redirect_error(asio_ns::use_awaitable, error));

    if (error == asio_ns::error::operation_aborted || !acceptor.is_open()) {
      co_return;
    }
    if (error) {
      continue;
    }

    asio_ns::co_spawn(
        executor,
        [socket = std::move(socket)]() mutable -> asio_ns::awaitable<void> {
          try {
            co_await serve_connection(std::move(socket));
          } catch (const std::exception&) {
            close_socket(socket);
          }
        },
        asio_ns::detached);
  }
}

// Signal handling is modeled as another coroutine so shutdown uses the same
// event loop and cancellation path as the network work.
asio_ns::awaitable<void> wait_for_shutdown(asio_ns::signal_set& signals,
                                           tcp::acceptor& acceptor,
                                           asio_ns::io_context& io_context) {
  std::error_code error;
  co_await signals.async_wait(asio_ns::redirect_error(asio_ns::use_awaitable, error));
  if (!error) {
    std::error_code ignored;
    acceptor.cancel(ignored);
    acceptor.close(ignored);
    io_context.stop();
  }
  co_return;
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
  asio_ns::io_context io_context(1);
  auto endpoint = resolve_bind_endpoint(io_context, config_);

  tcp::acceptor acceptor(io_context);
  acceptor.open(endpoint.protocol());
  acceptor.set_option(asio_ns::socket_base::reuse_address(true));
  acceptor.bind(endpoint);
  acceptor.listen(asio_ns::socket_base::max_listen_connections);

  asio_ns::signal_set signals(io_context, SIGINT, SIGTERM);
  asio_ns::co_spawn(io_context, accept_loop(acceptor), asio_ns::detached);
  asio_ns::co_spawn(
      io_context, wait_for_shutdown(signals, acceptor, io_context), asio_ns::detached);

  if (!config_.quiet) {
    const auto local = acceptor.local_endpoint();
    std::println("rpc-bench-server listening on {}",
                 Endpoint{
                     .host = config_.listen_host,
                     .port = local.port(),
                 }
                     .to_string());
  }

  io_context.run();
  return 0;
}

} // namespace rpcbench
