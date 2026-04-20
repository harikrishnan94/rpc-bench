// Benchmark runner for the CRC32 RPC service. The implementation keeps the
// measured workload closed-loop and single-endpoint while the transport layer
// now owns the per-transport attach and spawn-local lifecycle details.

#include "bench/benchmark.hpp"

#include "protocol/service.hpp"
#include "transport/benchmark_transport.hpp"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <kj/async-io.h>
#include <kj/exception.h>
#include <kj/string.h>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace rpcbench {
namespace {

using Clock = std::chrono::steady_clock;
inline constexpr std::uint64_t kReportedRequestEnvelopeBytes = 4;
inline constexpr std::uint64_t kReportedResponseEnvelopeBytes = 4;

struct ThreadStats {
  // These counters describe one client thread's measured contribution so the
  // main thread can aggregate throughput and latency without sharing mutable
  // state across worker connections.
  std::uint64_t total_requests = 0;
  std::uint64_t errors = 0;
  std::uint64_t request_bytes = 0;
  std::uint64_t response_bytes = 0;
  std::vector<std::uint64_t> latencies_ns;
  double measured_seconds = 0.0;
};

struct ThreadResult {
  // The worker thread returns both its local stats and an optional fatal error
  // string so the main thread can join everyone before deciding whether the
  // overall benchmark invocation succeeded.
  ThreadStats stats;
  std::optional<std::string> error;
};

struct PayloadGenerator {
  // The generator owns one deterministic stream per thread so runs can be
  // reproduced exactly when the same seed and thread count are used.
public:
  explicit PayloadGenerator(std::size_t thread_index, MessageSizeRange sizes, std::uint64_t seed)
      : rng(seed ^ ((thread_index + 1) * 0x9e3779b185ebca87ULL)),
        size_distribution(sizes.min, sizes.max), byte_distribution(0, 255) {}

  [[nodiscard]] std::vector<std::byte> next_payload() {
    const auto size = size_distribution(rng);
    std::vector<std::byte> payload(size);
    for (auto& byte : payload) {
      byte = std::byte(byte_distribution(rng));
    }
    return payload;
  }

private:
  std::mt19937_64 rng;
  std::uniform_int_distribution<std::size_t> size_distribution;
  std::uniform_int_distribution<unsigned int> byte_distribution;
};

// Formats KJ exceptions into stable user-facing strings for benchmark errors.
[[nodiscard]] std::string describe_kj_exception(const kj::Exception& exception) {
  return kj::str(exception).cStr();
}

[[nodiscard]] capnp::Data::Reader to_data_reader(std::span<const std::byte> payload) {
  return {
      reinterpret_cast<const capnp::byte*>(payload.data()),
      payload.size(),
  };
}

// Sends one unary RPC and waits for the full response before returning. The
// benchmark deliberately uses blocking waits at the thread top level so each
// thread keeps exactly one request in flight at a time.
[[nodiscard]] std::uint32_t send_hash_request(HashService::Client& hash_service,
                                              std::span<const std::byte> payload,
                                              kj::WaitScope& wait_scope) {
  auto request = hash_service.hashRequest();
  request.setPayload(to_data_reader(payload));
  return request.send().wait(wait_scope).getCrc32();
}

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

[[nodiscard]] double bytes_to_mib_per_second(std::uint64_t bytes, double seconds) {
  if (seconds <= 0.0) {
    return 0.0;
  }
  return (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds;
}

[[nodiscard]] std::string format_latency(std::uint64_t nanoseconds) {
  if (nanoseconds >= 1'000'000'000ULL) {
    return std::format("{:.2f}s", static_cast<double>(nanoseconds) / 1'000'000'000.0);
  }
  if (nanoseconds >= 1'000'000ULL) {
    return std::format("{:.2f}ms", static_cast<double>(nanoseconds) / 1'000'000.0);
  }
  if (nanoseconds >= 1'000ULL) {
    return std::format("{:.2f}us", static_cast<double>(nanoseconds) / 1'000.0);
  }
  return std::format("{}ns", nanoseconds);
}

[[nodiscard]] std::string json_escape(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size() + 8);

  for (const char ch : text) {
    switch (ch) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped.push_back(ch);
      break;
    }
  }

  return escaped;
}

[[nodiscard]] std::uint64_t nearest_rank(const std::vector<std::uint64_t>& samples,
                                         double percentile) {
  if (samples.empty()) {
    return 0;
  }

  const auto rank =
      static_cast<std::size_t>(std::ceil(percentile * static_cast<double>(samples.size())));
  const auto index = std::min(samples.size() - 1, std::max<std::size_t>(1, rank) - 1);
  return samples[index];
}

[[nodiscard]] LatencyPercentiles compute_percentiles(std::vector<std::uint64_t> samples) {
  if (samples.empty()) {
    return {};
  }

  std::sort(samples.begin(), samples.end());
  return LatencyPercentiles{
      .p50_ns = nearest_rank(samples, 0.500),
      .p75_ns = nearest_rank(samples, 0.750),
      .p90_ns = nearest_rank(samples, 0.900),
      .p99_ns = nearest_rank(samples, 0.990),
      .p999_ns = nearest_rank(samples, 0.999),
  };
}

void run_phase(HashService::Client& hash_service,
               PayloadGenerator& generator,
               double seconds,
               ThreadStats* stats,
               kj::WaitScope& wait_scope) {
  // Each phase is closed-loop: a thread sends the next request only after the
  // previous RPC reply has been received completely.
  const auto deadline = Clock::now() + std::chrono::duration<double>(seconds);
  const auto phase_start = Clock::now();

  while (Clock::now() < deadline) {
    auto payload = generator.next_payload();
    const auto start = Clock::now();
    static_cast<void>(send_hash_request(hash_service, payload, wait_scope));
    const auto end = Clock::now();

    if (stats != nullptr) {
      ++stats->total_requests;
      stats->request_bytes += kReportedRequestEnvelopeBytes + payload.size();
      stats->response_bytes += kReportedResponseEnvelopeBytes;
      stats->latencies_ns.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
    }
  }

  if (stats != nullptr) {
    stats->measured_seconds = std::chrono::duration<double>(Clock::now() - phase_start).count();
  }
}

[[nodiscard]] ThreadResult run_client_thread(const TransportUri& uri,
                                             const BenchConfig& config,
                                             WorkerAttachment attachment,
                                             std::size_t thread_index,
                                             std::barrier<>& warmup_barrier,
                                             std::barrier<>& measure_barrier) {
  bool joined_warmup = false;
  bool joined_measure = false;

  try {
    auto io_context = kj::setupAsyncIo();
    auto channel = std::move(attachment).open_channel(uri, io_context);
    if (!channel) {
      throw std::runtime_error(channel.error());
    }

    ClientRpcSession rpc_session(kj::mv(*channel));
    PayloadGenerator generator(thread_index, config.message_sizes, config.seed);
    warmup_barrier.arrive_and_wait();
    joined_warmup = true;
    run_phase(rpc_session.hash_service(),
              generator,
              config.warmup_seconds,
              nullptr,
              io_context.waitScope);

    measure_barrier.arrive_and_wait();
    joined_measure = true;
    ThreadResult result;
    run_phase(rpc_session.hash_service(),
              generator,
              config.measure_seconds,
              &result.stats,
              io_context.waitScope);
    return result;
  } catch (const kj::Exception& exception) {
    if (!joined_warmup) {
      warmup_barrier.arrive_and_drop();
      measure_barrier.arrive_and_drop();
    } else if (!joined_measure) {
      measure_barrier.arrive_and_drop();
    }

    return ThreadResult{
        .stats =
            ThreadStats{
                .errors = 1,
                .latencies_ns = {},
            },
        .error = std::format(
            "client thread {} failed: {}", thread_index, describe_kj_exception(exception)),
    };
  } catch (const std::exception& exception) {
    if (!joined_warmup) {
      warmup_barrier.arrive_and_drop();
      measure_barrier.arrive_and_drop();
    } else if (!joined_measure) {
      measure_barrier.arrive_and_drop();
    }

    return ThreadResult{
        .stats =
            ThreadStats{
                .errors = 1,
                .latencies_ns = {},
            },
        .error = std::format("client thread {} failed: {}", thread_index, exception.what()),
    };
  }
}

void append_json_string(std::string& out, std::string_view text) {
  out += '"';
  out += json_escape(text);
  out += '"';
}

[[nodiscard]] BenchmarkTransportConfig make_transport_config(const BenchConfig& config) {
  return BenchmarkTransportConfig{
      .mode = config.mode,
      .connect_uri = config.connect_uri,
      .server_binary = config.server_binary,
      .listen_uri = config.listen_uri,
      .client_threads = config.client_threads,
      .quiet_server = config.quiet_server,
      .startup_timeout_ms = config.startup_timeout_ms,
  };
}

} // namespace

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

std::expected<void, std::string> BenchConfig::validate() const {
  if (client_threads == 0) {
    return std::unexpected("client thread count must be greater than zero");
  }
  if (auto valid = message_sizes.validate(); !valid) {
    return valid;
  }
  if (warmup_seconds < 0.0) {
    return std::unexpected("warmup duration must be zero or greater");
  }
  if (measure_seconds <= 0.0) {
    return std::unexpected("measure duration must be greater than zero");
  }
  if (startup_timeout_ms == 0) {
    return std::unexpected("startup timeout must be greater than zero");
  }

  switch (mode) {
  case BenchMode::connect:
    if (!connect_uri) {
      return std::unexpected("connect mode requires --connect-uri=URI");
    }
    if (connect_uri->kind == TransportKind::pipe_socketpair) {
      return std::unexpected(
          "connect mode does not support pipe://socketpair because it requires inherited fds");
    }
    break;
  case BenchMode::spawn_local:
    if (server_binary.empty()) {
      return std::unexpected("spawn-local mode requires a server binary path");
    }
    break;
  }

  return {};
}

TransportUri BenchConfig::resolved_uri() const {
  if (mode == BenchMode::connect) {
    return connect_uri.value_or(listen_uri);
  }
  return listen_uri;
}

std::string BenchmarkResult::to_text() const {
  return std::format("rpc-bench report\n"
                     "mode: {}\n"
                     "endpoint: {}\n"
                     "clientThreads: {}\n"
                     "messageSizes: {}-{} bytes\n"
                     "measuredSeconds: {:.6f}\n"
                     "totalRequests: {}\n"
                     "errors: {}\n"
                     "requestBytes: {}\n"
                     "responseBytes: {}\n"
                     "requestsPerSecond: {:.2f}\n"
                     "requestMiBPerSecond: {:.2f}\n"
                     "responseMiBPerSecond: {:.2f}\n"
                     "combinedMiBPerSecond: {:.2f}\n"
                     "latency:\n"
                     "  p50: {}\n"
                     "  p75: {}\n"
                     "  p90: {}\n"
                     "  p99: {}\n"
                     "  p99.9: {}\n",
                     bench_mode_name(mode),
                     endpoint,
                     client_threads,
                     message_sizes.min,
                     message_sizes.max,
                     measured_seconds,
                     total_requests,
                     errors,
                     request_bytes,
                     response_bytes,
                     requests_per_second,
                     request_mib_per_second,
                     response_mib_per_second,
                     combined_mib_per_second,
                     format_latency(latency.p50_ns),
                     format_latency(latency.p75_ns),
                     format_latency(latency.p90_ns),
                     format_latency(latency.p99_ns),
                     format_latency(latency.p999_ns));
}

std::string BenchmarkResult::to_json() const {
  std::string json;
  json += "{\n";
  json += "  \"mode\": ";
  append_json_string(json, bench_mode_name(mode));
  json += ",\n";
  json += "  \"endpoint\": ";
  append_json_string(json, endpoint);
  json += ",\n";
  json += std::format("  \"clientThreads\": {},\n", client_threads);
  json += std::format("  \"messageSizeMin\": {},\n", message_sizes.min);
  json += std::format("  \"messageSizeMax\": {},\n", message_sizes.max);
  json += std::format("  \"measuredSeconds\": {:.6f},\n", measured_seconds);
  json += std::format("  \"totalRequests\": {},\n", total_requests);
  json += std::format("  \"errors\": {},\n", errors);
  json += std::format("  \"requestBytes\": {},\n", request_bytes);
  json += std::format("  \"responseBytes\": {},\n", response_bytes);
  json += std::format("  \"requestsPerSecond\": {:.6f},\n", requests_per_second);
  json += std::format("  \"requestMiBPerSecond\": {:.6f},\n", request_mib_per_second);
  json += std::format("  \"responseMiBPerSecond\": {:.6f},\n", response_mib_per_second);
  json += std::format("  \"combinedMiBPerSecond\": {:.6f},\n", combined_mib_per_second);
  json += "  \"latencyNs\": {\n";
  json += std::format("    \"p50\": {},\n", latency.p50_ns);
  json += std::format("    \"p75\": {},\n", latency.p75_ns);
  json += std::format("    \"p90\": {},\n", latency.p90_ns);
  json += std::format("    \"p99\": {},\n", latency.p99_ns);
  json += std::format("    \"p999\": {}\n", latency.p999_ns);
  json += "  }\n";
  json += "}\n";
  return json;
}

std::expected<BenchConfig, std::string> parse_bench_config(std::span<const std::string_view> args,
                                                           const std::filesystem::path& argv0) {
  BenchConfig config;
  config.server_binary = default_server_binary_path(argv0);

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
      continue;
    }

    if (const auto value = get_value(arg, "--client-threads=")) {
      auto parsed = parse_integer<unsigned long long>(*value, "client thread count");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.client_threads = static_cast<std::size_t>(*parsed);
      continue;
    }

    if (const auto value = get_value(arg, "--message-size-min=")) {
      auto parsed = parse_integer<unsigned long long>(*value, "message size minimum");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.message_sizes.min = static_cast<std::size_t>(*parsed);
      continue;
    }

    if (const auto value = get_value(arg, "--message-size-max=")) {
      auto parsed = parse_integer<unsigned long long>(*value, "message size maximum");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.message_sizes.max = static_cast<std::size_t>(*parsed);
      continue;
    }

    if (const auto value = get_value(arg, "--warmup-seconds=")) {
      auto parsed = parse_double(*value, "warmup duration");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.warmup_seconds = *parsed;
      continue;
    }

    if (const auto value = get_value(arg, "--measure-seconds=")) {
      auto parsed = parse_double(*value, "measure duration");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.measure_seconds = *parsed;
      continue;
    }

    if (const auto value = get_value(arg, "--seed=")) {
      auto parsed = parse_integer<std::uint64_t>(*value, "seed");
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      config.seed = *parsed;
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

    return std::unexpected(std::format("unknown benchmark argument '{}'", arg));
  }

  if (auto valid = config.validate(); !valid) {
    return std::unexpected(valid.error());
  }

  return config;
}

std::string bench_usage(std::string_view program_name) {
  return std::format(
      "Usage: {} [options]\n"
      "\n"
      "Options:\n"
      "  --mode=connect|spawn-local Run mode. Default: spawn-local\n"
      "  --connect-uri=URI         Target URI for connect mode\n"
      "  --server-binary=PATH      Server binary for spawn-local mode\n"
      "  --listen-uri=URI          Spawn-local listen URI. Default: tcp://127.0.0.1:7300\n"
      "  --client-threads=N        Client thread count. Default: 1\n"
      "  --message-size-min=N      Inclusive minimum payload size. Default: 128\n"
      "  --message-size-max=N      Inclusive maximum payload size. Default: 256\n"
      "  --warmup-seconds=SECONDS  Warmup duration. Default: 1.0\n"
      "  --measure-seconds=SECONDS Measured duration. Default: 3.0\n"
      "  --seed=N                  Deterministic payload seed. Default: 1\n"
      "  --startup-timeout-ms=N    Spawn-local startup timeout. Default: 5000\n"
      "  --json-output=PATH        Optional JSON report path\n"
      "  --quiet-server            Suppress the child server banner\n"
      "  --help                    Show this message\n",
      program_name);
}

std::expected<BenchmarkResult, std::string> run_benchmark(const BenchConfig& config) {
  auto prepared_transport = PreparedBenchmarkTransport::prepare(make_transport_config(config));
  if (!prepared_transport) {
    return std::unexpected(prepared_transport.error());
  }

  const auto uri = prepared_transport->resolved_uri();
  auto worker_attachments = prepared_transport->take_worker_attachments();
  std::barrier<> warmup_barrier(static_cast<std::ptrdiff_t>(config.client_threads + 1));
  std::barrier<> measure_barrier(static_cast<std::ptrdiff_t>(config.client_threads + 1));

  std::vector<ThreadResult> results(config.client_threads);
  std::vector<std::thread> threads;
  threads.reserve(config.client_threads);

  for (std::size_t index = 0; index < config.client_threads; ++index) {
    threads.emplace_back([&, index, attachment = std::move(worker_attachments[index])]() mutable {
      results[index] = run_client_thread(
          uri, config, std::move(attachment), index, warmup_barrier, measure_barrier);
    });
  }

  warmup_barrier.arrive_and_wait();
  measure_barrier.arrive_and_wait();

  for (auto& thread : threads) {
    thread.join();
  }

  BenchmarkResult result{
      .mode = config.mode,
      .endpoint = uri.to_string(),
      .client_threads = config.client_threads,
      .message_sizes = config.message_sizes,
      .latency = {},
  };

  std::vector<std::uint64_t> latencies;
  std::optional<std::string> first_error;
  for (const auto& thread_result : results) {
    result.total_requests += thread_result.stats.total_requests;
    result.errors += thread_result.stats.errors;
    result.request_bytes += thread_result.stats.request_bytes;
    result.response_bytes += thread_result.stats.response_bytes;
    result.measured_seconds =
        std::max(result.measured_seconds, thread_result.stats.measured_seconds);
    if (thread_result.error && !first_error) {
      first_error = *thread_result.error;
    }
    latencies.insert(latencies.end(),
                     thread_result.stats.latencies_ns.begin(),
                     thread_result.stats.latencies_ns.end());
  }

  if (first_error) {
    return std::unexpected(
        std::format("benchmark failed with {} client thread error(s); first error: {}",
                    result.errors,
                    *first_error));
  }
  if (const auto background_error = prepared_transport->background_error(); background_error) {
    return std::unexpected(
        std::format("transport background worker failed: {}", *background_error));
  }

  result.latency = compute_percentiles(std::move(latencies));
  if (result.measured_seconds > 0.0) {
    result.requests_per_second =
        static_cast<double>(result.total_requests) / result.measured_seconds;
    result.request_mib_per_second =
        bytes_to_mib_per_second(result.request_bytes, result.measured_seconds);
    result.response_mib_per_second =
        bytes_to_mib_per_second(result.response_bytes, result.measured_seconds);
    result.combined_mib_per_second = bytes_to_mib_per_second(
        result.request_bytes + result.response_bytes, result.measured_seconds);
  }

  return result;
}

} // namespace rpcbench
