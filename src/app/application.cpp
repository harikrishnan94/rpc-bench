// CLI parsing plus top-level orchestration for the server and benchmark
// executables. This is the only layer that knows about URI strings and run
// modes; the runtime layers below operate on async stream abstractions only.

#include "app/application.hpp"

#include "server/server.hpp"
#include "transport/benchmark_transport.hpp"
#include "transport/server_transport.hpp"

#include <format>

namespace rpcbench {
namespace {

[[nodiscard]] std::filesystem::path default_server_binary_path(const std::filesystem::path& argv0) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(argv0, error);
  const auto executable = error ? argv0 : absolute;
  const auto parent = executable.parent_path();

  // Meson's default layout places the benchmark under builddir/src/bench and
  // the server under builddir/src/server, so prefer that sibling-directory
  // lookup before falling back to the benchmark's own directory.
  if (parent.filename() == "bench" && parent.parent_path().filename() == "src") {
    return parent.parent_path() / "server" / "rpc-bench-server";
  }

  return sibling_binary_path(argv0, "rpc-bench-server");
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

[[nodiscard]] std::expected<void, std::string> validate_server_config(const ServerConfig& config) {
  if (config.server_threads == 0) {
    return std::unexpected("server thread count must be greater than zero");
  }

  switch (config.listen_uri.kind) {
  case TransportKind::tcp:
  case TransportKind::unix_socket:
    if (!config.preconnected_stream_fds.empty()) {
      return std::unexpected("internal preconnected fds are only valid with pipe://socketpair");
    }
    return {};
  case TransportKind::pipe_socketpair:
    if (config.preconnected_stream_fds.empty()) {
      return std::unexpected(
          "pipe://socketpair requires internal preconnected fds from spawn-local mode");
    }
    return {};
  case TransportKind::unspecified:
    return std::unexpected("listen URI must not be empty");
  }

  return std::unexpected("unsupported transport kind");
}

[[nodiscard]] std::expected<void, std::string> validate_bench_config(const BenchConfig& config,
                                                                     bool saw_server_threads,
                                                                     bool saw_connect_uri,
                                                                     bool saw_listen_uri) {
  if (config.server_threads == 0) {
    return std::unexpected("server thread count must be greater than zero");
  }
  if (config.startup_timeout_ms == 0) {
    return std::unexpected("startup timeout must be greater than zero");
  }
  if (config.benchmark.client_threads == 0) {
    return std::unexpected("client thread count must be greater than zero");
  }
  if (config.benchmark.client_connections == 0) {
    return std::unexpected("client connection count must be greater than zero");
  }
  if (auto valid = config.benchmark.message_sizes.validate(); !valid) {
    return valid;
  }
  if (config.benchmark.warmup_seconds < 0.0) {
    return std::unexpected("warmup duration must be zero or greater");
  }
  if (config.benchmark.measure_seconds <= 0.0) {
    return std::unexpected("measure duration must be greater than zero");
  }

  switch (config.mode) {
  case BenchMode::connect:
    if (!config.connect_uri) {
      return std::unexpected("connect mode requires --connect-uri=URI");
    }
    if (config.connect_uri->kind == TransportKind::pipe_socketpair) {
      return std::unexpected(
          "connect mode does not support pipe://socketpair because it requires inherited fds");
    }
    if (saw_listen_uri) {
      return std::unexpected("connect mode does not accept --listen-uri");
    }
    if (saw_server_threads) {
      return std::unexpected("connect mode does not accept --server-threads");
    }
    break;
  case BenchMode::spawn_local:
    if (config.server_binary.empty()) {
      return std::unexpected("spawn-local mode requires a server binary path");
    }
    if (saw_connect_uri) {
      return std::unexpected("spawn-local mode does not accept --connect-uri");
    }
    break;
  }

  return {};
}

[[nodiscard]] std::expected<std::size_t, std::string> parse_size_value(std::string_view text,
                                                                       std::string_view name) {
  auto parsed = parse_integer<unsigned long long>(text, name);
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  return static_cast<std::size_t>(*parsed);
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
    if (const auto value = get_value(arg, "--server-threads=")) {
      auto parsed = parse_size_value(*value, "server thread count");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.server_threads = *parsed;
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
    if (arg.starts_with("--listen-host=") || arg.starts_with("--port=")) {
      return std::unexpected("server now uses --listen-uri=URI instead of host/port flags");
    }
    return std::unexpected(std::format("unknown server argument '{}'", arg));
  }

  if (auto valid = validate_server_config(config); !valid) {
    return std::unexpected(valid.error());
  }
  return config;
}

std::string server_usage(std::string_view program_name) {
  return std::format("Usage: {} [options]\n"
                     "\n"
                     "Options:\n"
                     "  --listen-uri=URI          Listen target. Default: tcp://127.0.0.1:7000\n"
                     "  --server-threads=N        Requested server thread count. Default: 1\n"
                     "  --quiet                   Suppress the startup banner\n"
                     "  --help                    Show this message\n",
                     program_name);
}

int run_server_app(const ServerConfig& config) {
  ServerTransportConfig transport_config{
      .listen_uri = config.listen_uri,
      .server_threads = config.server_threads,
      .quiet = config.quiet,
      .ready_fd = config.ready_fd,
      .preconnected_stream_fds = config.preconnected_stream_fds,
  };
  return run_server_transport(transport_config, make_hash_service_bootstrap());
}

std::expected<BenchConfig, std::string> parse_bench_config(std::span<const std::string_view> args,
                                                           const std::filesystem::path& argv0) {
  BenchConfig config;
  config.server_binary = default_server_binary_path(argv0);

  bool saw_server_threads = false;
  bool saw_connect_uri = false;
  bool saw_listen_uri = false;

  for (const auto arg : args) {
    if (has_flag(arg, "--quiet-server")) {
      config.quiet_server = true;
      continue;
    }
    if (const auto value = get_value(arg, "--mode=")) {
      auto parsed = parse_bench_mode(*value);
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.mode = *parsed;
      continue;
    }
    if (const auto value = get_value(arg, "--connect-uri=")) {
      auto parsed = parse_transport_uri(*value);
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.connect_uri = *parsed;
      saw_connect_uri = true;
      continue;
    }
    if (const auto value = get_value(arg, "--server-binary=")) {
      config.server_binary = std::filesystem::path(*value);
      continue;
    }
    if (const auto value = get_value(arg, "--listen-uri=")) {
      auto parsed = parse_transport_uri(*value);
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.listen_uri = *parsed;
      saw_listen_uri = true;
      continue;
    }
    if (const auto value = get_value(arg, "--server-threads=")) {
      auto parsed = parse_size_value(*value, "server thread count");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.server_threads = *parsed;
      saw_server_threads = true;
      continue;
    }
    if (const auto value = get_value(arg, "--client-threads=")) {
      auto parsed = parse_size_value(*value, "client thread count");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.benchmark.client_threads = *parsed;
      continue;
    }
    if (const auto value = get_value(arg, "--client-connections=")) {
      auto parsed = parse_size_value(*value, "client connection count");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.benchmark.client_connections = *parsed;
      continue;
    }
    if (const auto value = get_value(arg, "--message-size-min=")) {
      auto parsed = parse_size_value(*value, "minimum message size");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.benchmark.message_sizes.min = *parsed;
      continue;
    }
    if (const auto value = get_value(arg, "--message-size-max=")) {
      auto parsed = parse_size_value(*value, "maximum message size");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.benchmark.message_sizes.max = *parsed;
      continue;
    }
    if (const auto value = get_value(arg, "--warmup-seconds=")) {
      auto parsed = parse_double(*value, "warmup duration");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.benchmark.warmup_seconds = *parsed;
      continue;
    }
    if (const auto value = get_value(arg, "--measure-seconds=")) {
      auto parsed = parse_double(*value, "measure duration");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.benchmark.measure_seconds = *parsed;
      continue;
    }
    if (const auto value = get_value(arg, "--seed=")) {
      auto parsed = parse_integer<unsigned long long>(*value, "seed");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.benchmark.seed = *parsed;
      continue;
    }
    if (const auto value = get_value(arg, "--json-output=")) {
      config.json_output = std::filesystem::path(*value);
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
    if (arg.starts_with("--queue-depths=")) {
      return std::unexpected(
          "--queue-depths is no longer supported; use --client-connections=N instead");
    }
    if (arg.starts_with("--endpoint=")) {
      return std::unexpected("benchmark now uses --connect-uri=URI instead of --endpoint");
    }
    if (arg.starts_with("--listen-host=") || arg.starts_with("--server-port=")) {
      return std::unexpected("benchmark now uses --listen-uri=URI instead of host/port flags");
    }
    return std::unexpected(std::format("unknown benchmark argument '{}'", arg));
  }

  if (auto valid =
          validate_bench_config(config, saw_server_threads, saw_connect_uri, saw_listen_uri);
      !valid) {
    return std::unexpected(valid.error());
  }
  return config;
}

std::string bench_usage(std::string_view program_name) {
  return std::format(
      "Usage: {} --mode=connect|spawn-local [options]\n"
      "\n"
      "Options:\n"
      "  --mode=connect|spawn-local Run mode. Default: spawn-local\n"
      "  --connect-uri=URI          Target URI for connect mode\n"
      "  --listen-uri=URI           Spawn-local listen URI. Default: tcp://127.0.0.1:7300\n"
      "  --server-binary=PATH       Server binary for spawn-local mode\n"
      "  --server-threads=N         Spawn-local server thread count. Default: 1\n"
      "  --client-threads=N         Number of benchmark event-loop threads. Default: 1\n"
      "  --client-connections=N     Total client connections. Default: 1\n"
      "  --message-size-min=N       Minimum request payload bytes. Default: 128\n"
      "  --message-size-max=N       Maximum request payload bytes. Default: 256\n"
      "  --warmup-seconds=SECONDS   Warmup duration. Default: 1.0\n"
      "  --measure-seconds=SECONDS  Measurement duration. Default: 3.0\n"
      "  --seed=N                   Deterministic payload seed. Default: 1\n"
      "  --json-output=PATH         Optional JSON output path\n"
      "  --startup-timeout-ms=N     Spawn-local startup timeout. Default: 5000\n"
      "  --quiet-server             Suppress the child server banner\n"
      "  --help                     Show this message\n",
      program_name);
}

std::expected<BenchmarkResult, std::string> run_benchmark_app(const BenchConfig& config) {
  auto prepared_transport = PreparedBenchmarkTransport::prepare(BenchmarkTransportConfig{
      .mode = config.mode,
      .connect_uri = config.connect_uri,
      .server_binary = config.server_binary,
      .listen_uri = config.listen_uri,
      .client_connections = config.benchmark.client_connections,
      .server_threads = config.server_threads,
      .quiet_server = config.quiet_server,
      .startup_timeout_ms = config.startup_timeout_ms,
  });
  if (!prepared_transport) {
    return std::unexpected(prepared_transport.error());
  }

  return run_benchmark(BenchmarkRunInput{
      .mode = std::string(bench_mode_name(config.mode)),
      .endpoint = prepared_transport->resolved_uri(),
      .options = config.benchmark,
      .connection_openers = prepared_transport->take_connection_openers(),
  });
}

} // namespace rpcbench
