// Shared URI parsing and path-derivation helpers. Keeping them in the
// transport layer lets the server, benchmark, and shared-memory sidecar logic
// agree on one transport namespace without duplicating parsing rules.

#include "transport/uri.hpp"

#include <cctype>
#include <cstdlib>
#include <exception>
#include <format>
#include <limits>

namespace rpcbench {
namespace {

[[nodiscard]] std::string format_host_port(std::string_view host, std::uint16_t port) {
  if (host.find(':') != std::string::npos && !host.starts_with('[')) {
    return std::format("[{}]:{}", host, port);
  }
  return std::format("{}:{}", host, port);
}

[[nodiscard]] std::expected<TransportUri, std::string> parse_tcp_uri_body(std::string_view text) {
  if (text.empty()) {
    return std::unexpected("tcp URI must include HOST:PORT");
  }

  if (text.starts_with('[')) {
    const auto close = text.rfind(']');
    if (close == std::string_view::npos || close + 2 >= text.size() || text[close + 1] != ':') {
      return std::unexpected(std::format("invalid tcp URI '{}'", text));
    }

    auto port = parse_port(text.substr(close + 2), "tcp port");
    if (!port) {
      return std::unexpected(port.error());
    }

    return TransportUri{
        .kind = TransportKind::tcp,
        .location = std::string(text.substr(1, close - 1)),
        .port = *port,
    };
  }

  const auto colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
    return std::unexpected(std::format("invalid tcp URI '{}'", text));
  }

  auto port = parse_port(text.substr(colon + 1), "tcp port");
  if (!port) {
    return std::unexpected(port.error());
  }

  return TransportUri{
      .kind = TransportKind::tcp,
      .location = std::string(text.substr(0, colon)),
      .port = *port,
  };
}

} // namespace

std::string TransportUri::to_string() const {
  switch (kind) {
  case TransportKind::unspecified:
    return {};
  case TransportKind::tcp:
    return std::format("tcp://{}", format_host_port(location, port));
  case TransportKind::unix_socket:
    return std::format("unix://{}", location);
  case TransportKind::pipe_socketpair:
    return "pipe://socketpair";
  case TransportKind::shared_memory:
    return std::format("shm://{}", location);
  }
  return {};
}

std::expected<std::string, std::string> TransportUri::to_kj_address() const {
  switch (kind) {
  case TransportKind::tcp:
    return format_host_port(location, port);
  case TransportKind::unix_socket:
    return std::format("unix:{}", location);
  case TransportKind::unspecified:
    return std::unexpected("transport URI is not configured");
  case TransportKind::pipe_socketpair:
  case TransportKind::shared_memory:
    return std::unexpected(std::format("{} does not use KJ NetworkAddress", to_string()));
  }
  return std::unexpected("unsupported transport kind");
}

bool TransportUri::uses_kj_network() const {
  return kind == TransportKind::tcp || kind == TransportKind::unix_socket;
}

std::expected<std::uint16_t, std::string> parse_port(std::string_view text, std::string_view name) {
  auto parsed = parse_integer<unsigned int>(text, name);
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  if (*parsed == 0 || *parsed > std::numeric_limits<std::uint16_t>::max()) {
    return std::unexpected(std::format("{} must be in the range 1-65535", name));
  }
  return static_cast<std::uint16_t>(*parsed);
}

std::expected<double, std::string> parse_double(std::string_view text, std::string_view name) {
  const std::string owned(text);
  std::size_t position = 0;

  try {
    const double value = std::stod(owned, &position);
    if (position != owned.size()) {
      return std::unexpected(std::format("invalid {}: '{}'", name, text));
    }
    return value;
  } catch (const std::exception&) {
    return std::unexpected(std::format("invalid {}: '{}'", name, text));
  }
}

std::expected<BenchMode, std::string> parse_bench_mode(std::string_view text) {
  if (text == "connect") {
    return BenchMode::connect;
  }
  if (text == "spawn-local") {
    return BenchMode::spawn_local;
  }
  return std::unexpected(std::format("invalid benchmark mode: '{}'", text));
}

std::expected<TransportUri, std::string> parse_transport_uri(std::string_view text) {
  if (text.empty()) {
    return std::unexpected("transport URI must not be empty");
  }

  constexpr std::string_view kTcpPrefix = "tcp://";
  constexpr std::string_view kUnixPrefix = "unix://";
  constexpr std::string_view kPipePrefix = "pipe://";
  constexpr std::string_view kShmPrefix = "shm://";

  if (text.starts_with(kTcpPrefix)) {
    return parse_tcp_uri_body(text.substr(kTcpPrefix.size()));
  }

  if (text.starts_with(kUnixPrefix)) {
    const auto path = text.substr(kUnixPrefix.size());
    if (path.empty() || !path.starts_with('/')) {
      return std::unexpected("unix URI must use an absolute path like unix:///tmp/rpc-bench.sock");
    }
    return TransportUri{
        .kind = TransportKind::unix_socket,
        .location = std::string(path),
    };
  }

  if (text.starts_with(kPipePrefix)) {
    if (text.substr(kPipePrefix.size()) != "socketpair") {
      return std::unexpected("the only supported pipe URI is pipe://socketpair");
    }
    return TransportUri{
        .kind = TransportKind::pipe_socketpair,
        .location = "socketpair",
    };
  }

  if (text.starts_with(kShmPrefix)) {
    const auto name = text.substr(kShmPrefix.size());
    if (name.empty()) {
      return std::unexpected("shared-memory URI must include a name like shm://bench");
    }
    if (name.find('/') != std::string_view::npos) {
      return std::unexpected("shared-memory URI names must not contain '/'");
    }
    return TransportUri{
        .kind = TransportKind::shared_memory,
        .location = std::string(name),
    };
  }

  return std::unexpected("transport URI must start with tcp://, unix://, pipe://, or shm://");
}

std::filesystem::path sibling_binary_path(const std::filesystem::path& argv0,
                                          std::string_view binary_name) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(argv0, error);
  const auto parent = error ? argv0.parent_path() : absolute.parent_path();
  return parent / binary_name;
}

std::filesystem::path derived_shm_sidecar_path(std::string_view logical_name) {
  std::string sanitized;
  sanitized.reserve(logical_name.size());
  for (const unsigned char ch : logical_name) {
    if (std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_') {
      sanitized.push_back(static_cast<char>(ch));
    } else {
      sanitized.push_back('_');
    }
  }
  if (sanitized.empty()) {
    sanitized = "bench";
  }

  const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
  const std::filesystem::path base_dir = runtime_dir != nullptr && runtime_dir[0] != '\0'
                                             ? std::filesystem::path(runtime_dir)
                                             : std::filesystem::path("/tmp");
  return base_dir / std::format("rpc-bench-shm-{}.sock", sanitized);
}

std::string_view bench_mode_name(BenchMode mode) {
  switch (mode) {
  case BenchMode::connect:
    return "connect";
  case BenchMode::spawn_local:
    return "spawn-local";
  }
  return "unknown";
}

} // namespace rpcbench
