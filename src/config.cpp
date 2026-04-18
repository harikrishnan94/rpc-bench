// Command-line parsing and validation for the runnable frontends. The parsing
// code stays intentionally strict so benchmark runs fail fast on ambiguous or
// misspelled flags instead of silently drifting away from the requested matrix.

#include "rpcbench/config.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <limits>
#include <ranges>
#include <sstream>

namespace rpcbench {
namespace {

template <typename Integer>
std::expected<Integer, std::string> parse_integer(std::string_view text, std::string_view name) {
  Integer value{};
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto [ptr, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || ptr != end) {
    return std::unexpected(std::format("invalid {}: '{}'", name, text));
  }
  return value;
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

std::vector<std::string_view> split_list(std::string_view text, char delimiter) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;

  while (start <= text.size()) {
    const auto end = text.find(delimiter, start);
    if (end == std::string_view::npos) {
      parts.push_back(text.substr(start));
      break;
    }
    parts.push_back(text.substr(start, end - start));
    start = end + 1;
  }

  return parts;
}

template <typename Integer>
std::expected<std::vector<std::size_t>, std::string> parse_size_list(std::string_view text,
                                                                     const char* name) {
  std::vector<std::size_t> values;

  for (const auto part : split_list(text, ',')) {
    if (part.empty()) {
      return std::unexpected(std::format("invalid {} list: empty entry", name));
    }

    auto parsed = parse_integer<Integer>(part, name);
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    if (*parsed == 0) {
      return std::unexpected(std::format("{} values must be greater than zero", name));
    }

    values.push_back(static_cast<std::size_t>(*parsed));
  }

  if (values.empty()) {
    return std::unexpected(std::format("{} list must not be empty", name));
  }

  return values;
}

std::expected<std::vector<std::string>, std::string> parse_string_list(std::string_view text,
                                                                       const char* name) {
  std::vector<std::string> values;

  for (const auto part : split_list(text, ',')) {
    if (part.empty()) {
      return std::unexpected(std::format("invalid {} list: empty entry", name));
    }
    values.emplace_back(part);
  }

  if (values.empty()) {
    return std::unexpected(std::format("{} list must not be empty", name));
  }

  return values;
}

std::expected<std::vector<OperationMix>, std::string> parse_mix_list(std::string_view text) {
  std::vector<OperationMix> mixes;

  for (const auto entry : split_list(text, ',')) {
    const auto fields = split_list(entry, ':');
    if (fields.size() != 3) {
      return std::unexpected(
          std::format("invalid mix '{}': expected get:put:delete percentages", entry));
    }

    auto get = parse_integer<std::uint32_t>(fields[0], "mix get percentage");
    auto put = parse_integer<std::uint32_t>(fields[1], "mix put percentage");
    auto del = parse_integer<std::uint32_t>(fields[2], "mix delete percentage");

    if (!get) {
      return std::unexpected(get.error());
    }
    if (!put) {
      return std::unexpected(put.error());
    }
    if (!del) {
      return std::unexpected(del.error());
    }

    mixes.push_back(OperationMix{
        .get_percent = *get,
        .put_percent = *put,
        .delete_percent = *del,
    });
  }

  if (mixes.empty()) {
    return std::unexpected("mix list must not be empty");
  }

  return mixes;
}

bool has_flag(std::string_view arg, std::string_view flag) {
  return arg == flag;
}

std::optional<std::string_view> get_value(std::string_view arg, std::string_view prefix) {
  if (!arg.starts_with(prefix)) {
    return std::nullopt;
  }
  return arg.substr(prefix.size());
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

std::filesystem::path default_server_binary(const std::filesystem::path& argv0) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(argv0, error);
  const auto parent = error ? argv0.parent_path() : absolute.parent_path();
  return parent / "rpc-bench-server";
}

std::expected<void, std::string> validate_mix(const OperationMix& mix) {
  const auto total = mix.get_percent + mix.put_percent + mix.delete_percent;
  if (total != 100) {
    return std::unexpected(
        std::format("operation mix {} is invalid: percentages must add up to 100", mix.label()));
  }
  return {};
}

std::string bench_mode_name(BenchMode mode) {
  switch (mode) {
  case BenchMode::connect:
    return "connect";
  case BenchMode::spawn_local:
    return "spawn-local";
  }
  return "unknown";
}

} // namespace

std::string OperationMix::label() const {
  return std::format("{}:{}:{}", get_percent, put_percent, delete_percent);
}

std::expected<void, std::string> WorkloadSpec::validate() const {
  const auto all_positive = [](const auto& values,
                               std::string_view name) -> std::expected<void, std::string> {
    if (values.empty()) {
      return std::unexpected(std::format("{} list must not be empty", name));
    }
    if (std::ranges::any_of(values, [](const auto value) { return value == 0; })) {
      return std::unexpected(std::format("{} values must be greater than zero", name));
    }
    return {};
  };

  if (auto valid = all_positive(client_threads, "client thread"); !valid) {
    return valid;
  }
  if (auto valid = all_positive(queue_depths, "queue depth"); !valid) {
    return valid;
  }
  if (auto valid = all_positive(key_sizes, "key size"); !valid) {
    return valid;
  }
  if (auto valid = all_positive(value_sizes, "value size"); !valid) {
    return valid;
  }
  if (key_space == 0) {
    return std::unexpected("key space must be greater than zero");
  }
  if (mixes.empty()) {
    return std::unexpected("at least one operation mix must be configured");
  }
  for (const auto& mix : mixes) {
    if (auto valid = validate_mix(mix); !valid) {
      return valid;
    }
  }
  if (warmup_seconds < 0.0) {
    return std::unexpected("warmup duration must be zero or greater");
  }
  if (measure_seconds <= 0.0) {
    return std::unexpected("measure duration must be greater than zero");
  }
  if (iterations == 0) {
    return std::unexpected("iteration count must be greater than zero");
  }

  return {};
}

std::string ServerConfig::bind_address() const {
  return std::format("{}:{}", listen_host, port);
}

std::expected<void, std::string> BenchConfig::validate() const {
  if (auto valid = workload.validate(); !valid) {
    return valid;
  }
  if (startup_timeout_ms == 0) {
    return std::unexpected("startup timeout must be greater than zero");
  }

  switch (mode) {
  case BenchMode::connect:
    if (endpoints.empty()) {
      return std::unexpected("connect mode requires at least one endpoint");
    }
    break;
  case BenchMode::spawn_local:
    if (server_workers.empty()) {
      return std::unexpected("spawn-local mode requires at least one server worker count");
    }
    if (std::ranges::any_of(server_workers, [](const auto value) { return value == 0; })) {
      return std::unexpected("server worker counts must be greater than zero");
    }
    if (base_port == 0) {
      return std::unexpected("base port must be in the range 1-65535");
    }
    if (!server_workers.empty()) {
      const auto max_workers = *std::ranges::max_element(server_workers);
      const auto last_port = static_cast<std::uint64_t>(base_port) + max_workers - 1;
      if (last_port > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected("spawn-local worker counts exceed the available port range");
      }
    }
    if (server_binary.empty()) {
      return std::unexpected("spawn-local mode requires a server binary path");
    }
    break;
  }

  return {};
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

  return config;
}

std::expected<BenchConfig, std::string> parse_bench_config(std::span<const std::string_view> args,
                                                           const std::filesystem::path& argv0) {
  BenchConfig config;
  config.server_binary = default_server_binary(argv0);

  for (const auto arg : args) {
    if (has_flag(arg, "--quiet-server")) {
      config.quiet_server = true;
      continue;
    }

    if (const auto value = get_value(arg, "--mode=")) {
      if (*value == "connect") {
        config.mode = BenchMode::connect;
      } else if (*value == "spawn-local") {
        config.mode = BenchMode::spawn_local;
      } else {
        return std::unexpected(std::format("invalid benchmark mode '{}'", *value));
      }
      continue;
    }

    if (const auto value = get_value(arg, "--endpoints=")) {
      auto parsed = parse_string_list(*value, "endpoint");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.endpoints = std::move(*parsed);
      continue;
    }

    if (const auto value = get_value(arg, "--server-binary=")) {
      config.server_binary = std::filesystem::path(std::string(*value));
      continue;
    }

    if (const auto value = get_value(arg, "--listen-host=")) {
      config.listen_host = std::string(*value);
      continue;
    }

    if (const auto value = get_value(arg, "--base-port=")) {
      auto parsed = parse_port(*value, "base port");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.base_port = *parsed;
      continue;
    }

    if (const auto value = get_value(arg, "--server-workers=")) {
      auto parsed = parse_size_list<unsigned int>(*value, "server worker");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.server_workers = std::move(*parsed);
      continue;
    }

    if (const auto value = get_value(arg, "--client-threads=")) {
      auto parsed = parse_size_list<unsigned int>(*value, "client thread");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.workload.client_threads = std::move(*parsed);
      continue;
    }

    if (const auto value = get_value(arg, "--queue-depths=")) {
      auto parsed = parse_size_list<unsigned int>(*value, "queue depth");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.workload.queue_depths = std::move(*parsed);
      continue;
    }

    if (const auto value = get_value(arg, "--key-sizes=")) {
      auto parsed = parse_size_list<unsigned int>(*value, "key size");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.workload.key_sizes = std::move(*parsed);
      continue;
    }

    if (const auto value = get_value(arg, "--value-sizes=")) {
      auto parsed = parse_size_list<unsigned int>(*value, "value size");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.workload.value_sizes = std::move(*parsed);
      continue;
    }

    if (const auto value = get_value(arg, "--mixes=")) {
      auto parsed = parse_mix_list(*value);
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.workload.mixes = std::move(*parsed);
      continue;
    }

    if (const auto value = get_value(arg, "--key-space=")) {
      auto parsed = parse_integer<unsigned int>(*value, "key space");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.workload.key_space = *parsed;
      continue;
    }

    if (const auto value = get_value(arg, "--warmup-seconds=")) {
      auto parsed = parse_double(*value, "warmup duration");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.workload.warmup_seconds = *parsed;
      continue;
    }

    if (const auto value = get_value(arg, "--measure-seconds=")) {
      auto parsed = parse_double(*value, "measure duration");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.workload.measure_seconds = *parsed;
      continue;
    }

    if (const auto value = get_value(arg, "--iterations=")) {
      auto parsed = parse_integer<unsigned int>(*value, "iteration count");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.workload.iterations = *parsed;
      continue;
    }

    if (const auto value = get_value(arg, "--seed=")) {
      auto parsed = parse_integer<std::uint64_t>(*value, "seed");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.workload.seed = *parsed;
      continue;
    }

    if (const auto value = get_value(arg, "--startup-timeout-ms=")) {
      auto parsed = parse_integer<unsigned int>(*value, "startup timeout");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.startup_timeout_ms = *parsed;
      continue;
    }

    if (const auto value = get_value(arg, "--json-output=")) {
      config.json_output = std::filesystem::path(std::string(*value));
      continue;
    }

    return std::unexpected(std::format("unknown benchmark argument '{}'", arg));
  }

  if (auto valid = config.validate(); !valid) {
    return std::unexpected(std::format(
        "invalid {} benchmark configuration: {}", bench_mode_name(config.mode), valid.error()));
  }

  return config;
}

std::string server_usage(std::string_view program_name) {
  return std::format("{} [--listen-host=HOST] [--port=PORT] [--quiet]\n"
                     "\n"
                     "Options:\n"
                     "  --listen-host=HOST   Host or address to bind. Default: 127.0.0.1\n"
                     "  --port=PORT          TCP port to bind. Default: 7000\n"
                     "  --quiet              Suppress the startup banner\n",
                     program_name);
}

std::string bench_usage(std::string_view program_name) {
  std::ostringstream out;
  out << std::format("{} --mode=connect|spawn-local [options]\n"
                     "\n"
                     "Mode-independent options:\n"
                     "  --client-threads=LIST      Comma-separated client thread counts\n"
                     "  --queue-depths=LIST        Comma-separated per-thread queue depths\n"
                     "  --key-sizes=LIST           Comma-separated key sizes in bytes\n"
                     "  --value-sizes=LIST         Comma-separated value sizes in bytes\n"
                     "  --mixes=LIST               Comma-separated get:put:delete percentages\n"
                     "  --key-space=N              Thread-local logical key count\n"
                     "  --warmup-seconds=N         Warmup duration in seconds\n"
                     "  --measure-seconds=N        Measured duration in seconds\n"
                     "  --iterations=N             Iterations per matrix point\n"
                     "  --seed=N                   Base random seed\n"
                     "  --json-output=PATH         Write JSON report to PATH\n"
                     "\n"
                     "Connect mode options:\n"
                     "  --endpoints=LIST           Comma-separated host:port endpoints\n"
                     "\n"
                     "Spawn-local mode options:\n"
                     "  --server-binary=PATH       Server binary path\n"
                     "  --listen-host=HOST         Host used for spawned endpoints\n"
                     "  --base-port=PORT           First port used for spawned endpoints\n"
                     "  --server-workers=LIST      Comma-separated worker counts to sweep\n"
                     "  --startup-timeout-ms=N     Spawn readiness timeout in milliseconds\n"
                     "  --quiet-server             Suppress child server logs\n",
                     program_name);

  return out.str();
}

} // namespace rpcbench
