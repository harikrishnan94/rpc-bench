#pragma once

// Shared protocol helpers for the clean-slate TCP benchmark. This header keeps
// wire-size limits, endpoint parsing, and small CLI parsing utilities in one
// internal place so the server and benchmark agree on the same contract.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace rpcbench {

inline constexpr std::size_t kDefaultMessageSizeMin = 128;
inline constexpr std::size_t kDefaultMessageSizeMax = 256;
inline constexpr std::uint32_t kMaxFrameSizeBytes = 1024U * 1024U;

enum class BenchMode : std::uint8_t {
  connect,
  spawn_local,
};

struct Endpoint {
  // TCP endpoint used by the benchmark client or server child management. The
  // host field is preserved exactly as configured so text reports stay stable.
  std::string host;
  std::uint16_t port = 0;

  // Returns a stable human-readable endpoint string. IPv6 literals are wrapped
  // in brackets so the result can round-trip through the parser.
  [[nodiscard]] std::string to_string() const;
};

struct MessageSizeRange {
  // Inclusive lower and upper request-payload bounds in bytes. Zero-length
  // payloads are allowed when explicitly requested, but the range may never
  // exceed the protocol frame size limit.
  std::size_t min = kDefaultMessageSizeMin;
  std::size_t max = kDefaultMessageSizeMax;

  // Validates ordering and the hard protocol frame cap.
  [[nodiscard]] std::expected<void, std::string> validate() const;
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

// Parses one endpoint string in HOST:PORT or [IPv6]:PORT form.
[[nodiscard]] std::expected<Endpoint, std::string> parse_endpoint(std::string_view text);

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
