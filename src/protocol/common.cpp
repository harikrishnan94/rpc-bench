// Shared protocol-side parsing and endpoint utilities. These helpers keep the
// server and benchmark frontends aligned on mode names, endpoint syntax, and
// the hard payload-size contract.

#include "protocol/common.hpp"

#include <exception>
#include <format>
#include <limits>

namespace rpcbench {

std::string Endpoint::to_string() const {
  if (host.find(':') != std::string::npos && !host.starts_with('[')) {
    return std::format("[{}]:{}", host, port);
  }
  return std::format("{}:{}", host, port);
}

std::expected<void, std::string> MessageSizeRange::validate() const {
  if (min > max) {
    return std::unexpected("message-size-min must be less than or equal to message-size-max");
  }
  if (max > kMaxPayloadSizeBytes) {
    return std::unexpected(
        std::format("message sizes must be less than or equal to {} bytes", kMaxPayloadSizeBytes));
  }
  return {};
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

std::expected<Endpoint, std::string> parse_endpoint(std::string_view text) {
  if (text.empty()) {
    return std::unexpected("endpoint must not be empty");
  }

  if (text.starts_with('[')) {
    const auto close = text.rfind(']');
    if (close == std::string_view::npos || close + 2 >= text.size() || text[close + 1] != ':') {
      return std::unexpected(std::format("invalid endpoint '{}'", text));
    }

    auto port = parse_port(text.substr(close + 2), "endpoint port");
    if (!port) {
      return std::unexpected(port.error());
    }

    return Endpoint{
        .host = std::string(text.substr(1, close - 1)),
        .port = *port,
    };
  }

  const auto colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
    return std::unexpected(std::format("invalid endpoint '{}'", text));
  }

  auto port = parse_port(text.substr(colon + 1), "endpoint port");
  if (!port) {
    return std::unexpected(port.error());
  }

  return Endpoint{
      .host = std::string(text.substr(0, colon)),
      .port = *port,
  };
}

std::filesystem::path sibling_binary_path(const std::filesystem::path& argv0,
                                          std::string_view binary_name) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(argv0, error);
  const auto parent = error ? argv0.parent_path() : absolute.parent_path();
  return parent / binary_name;
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
