#pragma once

// Shared transport-URI and small CLI parsing helpers for the application and
// transport layers. This module owns URI syntax only; the bench and server
// runtime layers operate on async stream abstractions instead.

#include <charconv>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace rpcbench {

enum class BenchMode : std::uint8_t {
  connect,
  spawn_local,
};

enum class TransportKind : std::uint8_t {
  unspecified,
  tcp,
  unix_socket,
  pipe_socketpair,
};

struct TransportUri {
  // Unified transport target shared by the benchmark and server CLIs. TCP
  // stores host + port and Unix sockets store an absolute path. The only
  // non-network local transport in this milestone is `pipe://socketpair`.
  TransportKind kind = TransportKind::unspecified;
  std::string location;
  std::uint16_t port = 0;

  // Returns the stable user-facing URI string.
  [[nodiscard]] std::string to_string() const;

  // Returns the equivalent KJ network address for stream-backed transports.
  [[nodiscard]] std::expected<std::string, std::string> to_kj_address() const;

  // Returns true when the URI is implemented through KJ's NetworkAddress API.
  [[nodiscard]] bool uses_kj_network() const;
};

template <typename Integer>
std::expected<Integer, std::string> parse_integer(std::string_view text, std::string_view name) {
  Integer value{};
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto [ptr, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || ptr != end) {
    return std::unexpected(std::string("invalid ") + std::string(name) + ": '" + std::string(text) +
                           "'");
  }
  return value;
}

// Parses a TCP port and enforces the valid 1-65535 range.
[[nodiscard]] std::expected<std::uint16_t, std::string> parse_port(std::string_view text,
                                                                   std::string_view name);

// Parses a floating-point CLI value without applying sign or range policy.
[[nodiscard]] std::expected<double, std::string> parse_double(std::string_view text,
                                                              std::string_view name);

// Parses the benchmark mode token accepted by the benchmark CLI.
[[nodiscard]] std::expected<BenchMode, std::string> parse_bench_mode(std::string_view text);

// Parses one user-facing transport URI.
[[nodiscard]] std::expected<TransportUri, std::string> parse_transport_uri(std::string_view text);

// Resolves a sibling binary next to the current executable path.
[[nodiscard]] std::filesystem::path sibling_binary_path(const std::filesystem::path& argv0,
                                                        std::string_view binary_name);

// Returns the stable CLI/report string for one benchmark mode.
[[nodiscard]] std::string_view bench_mode_name(BenchMode mode);

// Small parser helpers shared by both frontends.
[[nodiscard]] inline bool has_flag(std::string_view arg, std::string_view flag) {
  return arg == flag;
}

[[nodiscard]] inline std::optional<std::string_view> get_value(std::string_view arg,
                                                               std::string_view prefix) {
  if (!arg.starts_with(prefix)) {
    return std::nullopt;
  }
  return arg.substr(prefix.size());
}

} // namespace rpcbench
