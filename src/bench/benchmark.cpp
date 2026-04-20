// Benchmark runtime for the CRC32 RPC service. This implementation keeps the
// workload closed-loop while letting each event-loop thread host multiple
// connections as independent coroutines with exactly one in-flight request per
// connection.

#include "bench/benchmark.hpp"

#include "protocol/service.hpp"
#include "transport/rpc_session.hpp"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <iterator>
#include <kj/async-io.h>
#include <kj/exception.h>
#include <kj/string.h>
#include <memory>
#include <optional>
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

struct ConnectionStats {
  // These counters describe one logical client connection's measured
  // contribution so the owning event-loop thread can aggregate without shared
  // mutable state across coroutines.
  std::uint64_t total_requests = 0;
  std::uint64_t request_bytes = 0;
  std::uint64_t response_bytes = 0;
  std::vector<std::uint64_t> latencies_ns;
};

struct ThreadStats {
  // These counters describe one event-loop thread's measured contribution so
  // the main thread can aggregate throughput and latency across thread-local
  // result buffers only after all work is finished.
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

struct ThreadConfig {
  // One event-loop thread owns one contiguous slice of the total connection
  // set.
  std::size_t thread_index = 0;
  std::size_t base_connection_index = 0;
  BenchmarkOptions options;
  std::vector<kj::Own<ConnectionOpener>> connection_openers;
};

struct PayloadGenerator {
  // The generator owns one deterministic stream per logical connection so runs
  // can be reproduced exactly when the same seed and connection layout are
  // used.
public:
  explicit PayloadGenerator(std::size_t connection_index,
                            MessageSizeRange sizes,
                            std::uint64_t seed)
      : rng(seed ^ ((connection_index + 1) * 0x9e3779b185ebca87ULL)),
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

struct OpenedConnection {
  // Each opened connection owns one stream-backed RPC session plus its own
  // deterministic payload generator.
  std::unique_ptr<ClientRpcSession> rpc_session;
  PayloadGenerator generator;
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

// Sends one unary RPC and waits for the full response before returning.
[[nodiscard]] kj::Promise<void> send_hash_request(HashService::Client& hash_service,
                                                  std::span<const std::byte> payload) {
  auto request = hash_service.hashRequest();
  request.setPayload(to_data_reader(payload));
  auto response = co_await request.send();
  static_cast<void>(response.getCrc32());
  co_return;
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

[[nodiscard]] kj::Promise<ConnectionStats>
run_connection_phase(OpenedConnection& connection, double seconds, bool measure_latency) {
  // Each connection is closed-loop: it sends the next request only after the
  // previous RPC reply has been received completely.
  const auto deadline = Clock::now() + std::chrono::duration<double>(seconds);
  ConnectionStats stats;

  while (Clock::now() < deadline) {
    auto payload = connection.generator.next_payload();
    const auto start = Clock::now();
    co_await send_hash_request(connection.rpc_session->hash_service(), payload);
    const auto end = Clock::now();

    ++stats.total_requests;
    stats.request_bytes += kReportedRequestEnvelopeBytes + payload.size();
    stats.response_bytes += kReportedResponseEnvelopeBytes;
    if (measure_latency) {
      stats.latencies_ns.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
    }
  }

  co_return stats;
}

[[nodiscard]] kj::Promise<ThreadStats>
run_thread_phase(std::vector<OpenedConnection>& connections, double seconds, bool measure_latency) {
  ThreadStats aggregated;
  if (connections.empty()) {
    co_return aggregated;
  }

  const auto phase_start = Clock::now();
  kj::Vector<kj::Promise<ConnectionStats>> tasks;
  tasks.reserve(connections.size());
  for (auto& connection : connections) {
    tasks.add(run_connection_phase(connection, seconds, measure_latency));
  }

  auto per_connection = co_await kj::joinPromises(tasks.releaseAsArray());
  for (auto& stats : per_connection) {
    aggregated.total_requests += stats.total_requests;
    aggregated.request_bytes += stats.request_bytes;
    aggregated.response_bytes += stats.response_bytes;
    aggregated.latencies_ns.insert(aggregated.latencies_ns.end(),
                                   std::make_move_iterator(stats.latencies_ns.begin()),
                                   std::make_move_iterator(stats.latencies_ns.end()));
  }
  aggregated.measured_seconds = std::chrono::duration<double>(Clock::now() - phase_start).count();
  co_return aggregated;
}

[[nodiscard]] kj::Promise<std::vector<OpenedConnection>>
open_connections(std::vector<kj::Own<ConnectionOpener>>& openers,
                 std::size_t base_connection_index,
                 const BenchmarkOptions& options,
                 kj::AsyncIoContext& io_context) {
  std::vector<OpenedConnection> connections;
  connections.reserve(openers.size());

  for (std::size_t index = 0; index < openers.size(); ++index) {
    auto stream = co_await openers[index]->open(io_context);
    connections.push_back(OpenedConnection{
        .rpc_session = std::make_unique<ClientRpcSession>(kj::mv(stream)),
        .generator =
            PayloadGenerator(base_connection_index + index, options.message_sizes, options.seed),
    });
  }

  co_return connections;
}

[[nodiscard]] ThreadResult run_client_thread(ThreadConfig config,
                                             std::barrier<>& warmup_barrier,
                                             std::barrier<>& measure_barrier) {
  bool joined_warmup = false;
  bool joined_measure = false;

  try {
    auto io_context = kj::setupAsyncIo();
    auto connections =
        open_connections(
            config.connection_openers, config.base_connection_index, config.options, io_context)
            .wait(io_context.waitScope);
    warmup_barrier.arrive_and_wait();
    joined_warmup = true;
    run_thread_phase(connections, config.options.warmup_seconds, false).wait(io_context.waitScope);

    measure_barrier.arrive_and_wait();
    joined_measure = true;
    return ThreadResult{
        .stats = run_thread_phase(connections, config.options.measure_seconds, true)
                     .wait(io_context.waitScope),
        .error = std::nullopt,
    };
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
            "client thread {} failed: {}", config.thread_index, describe_kj_exception(exception)),
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
        .error = std::format("client thread {} failed: {}", config.thread_index, exception.what()),
    };
  }
}

void append_json_string(std::string& out, std::string_view text) {
  out += '"';
  out += json_escape(text);
  out += '"';
}

[[nodiscard]] std::vector<ThreadConfig> split_connection_openers(BenchmarkRunInput& input) {
  std::vector<ThreadConfig> thread_configs(input.options.client_threads);
  const std::size_t base_count = input.options.client_connections / input.options.client_threads;
  const std::size_t extra = input.options.client_connections % input.options.client_threads;

  std::size_t cursor = 0;
  for (std::size_t thread_index = 0; thread_index < input.options.client_threads; ++thread_index) {
    const std::size_t count = base_count + (thread_index < extra ? 1 : 0);
    auto& config = thread_configs[thread_index];
    config.thread_index = thread_index;
    config.base_connection_index = cursor;
    config.options = input.options;
    config.connection_openers.reserve(count);

    for (std::size_t local = 0; local < count; ++local) {
      config.connection_openers.push_back(kj::mv(input.connection_openers[cursor]));
      ++cursor;
    }
  }

  return thread_configs;
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

std::string BenchmarkResult::to_text() const {
  return std::format("Mode: {}\n"
                     "Endpoint: {}\n"
                     "Client threads: {}\n"
                     "Client connections: {}\n"
                     "Message sizes: {}-{} bytes\n"
                     "Total requests: {}\n"
                     "Errors: {}\n"
                     "Request bytes: {}\n"
                     "Response bytes: {}\n"
                     "Measured seconds: {:.6f}\n"
                     "Requests/sec: {:.2f}\n"
                     "Request throughput: {:.2f} MiB/s\n"
                     "Response throughput: {:.2f} MiB/s\n"
                     "Combined throughput: {:.2f} MiB/s\n"
                     "Latency p50/p75/p90/p99/p99.9: {}, {}, {}, {}, {}\n",
                     mode,
                     endpoint,
                     client_threads,
                     client_connections,
                     message_sizes.min,
                     message_sizes.max,
                     total_requests,
                     errors,
                     request_bytes,
                     response_bytes,
                     measured_seconds,
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
  std::string out;
  out.reserve(512);
  out += '{';
  out += "\"mode\":";
  append_json_string(out, mode);
  out += ',';
  out += "\"endpoint\":";
  append_json_string(out, endpoint);
  out += ',';
  out += "\"clientThreads\":";
  out += std::to_string(client_threads);
  out += ',';
  out += "\"clientConnections\":";
  out += std::to_string(client_connections);
  out += ',';
  out += "\"messageSizeMin\":";
  out += std::to_string(message_sizes.min);
  out += ',';
  out += "\"messageSizeMax\":";
  out += std::to_string(message_sizes.max);
  out += ',';
  out += "\"totalRequests\":";
  out += std::to_string(total_requests);
  out += ',';
  out += "\"errors\":";
  out += std::to_string(errors);
  out += ',';
  out += "\"requestBytes\":";
  out += std::to_string(request_bytes);
  out += ',';
  out += "\"responseBytes\":";
  out += std::to_string(response_bytes);
  out += ',';
  out += "\"measuredSeconds\":";
  out += std::format("{:.6f}", measured_seconds);
  out += ',';
  out += "\"requestsPerSecond\":";
  out += std::format("{:.6f}", requests_per_second);
  out += ',';
  out += "\"requestMiBPerSecond\":";
  out += std::format("{:.6f}", request_mib_per_second);
  out += ',';
  out += "\"responseMiBPerSecond\":";
  out += std::format("{:.6f}", response_mib_per_second);
  out += ',';
  out += "\"combinedMiBPerSecond\":";
  out += std::format("{:.6f}", combined_mib_per_second);
  out += ',';
  out += "\"latency\":{";
  out += "\"p50Ns\":";
  out += std::to_string(latency.p50_ns);
  out += ',';
  out += "\"p75Ns\":";
  out += std::to_string(latency.p75_ns);
  out += ',';
  out += "\"p90Ns\":";
  out += std::to_string(latency.p90_ns);
  out += ',';
  out += "\"p99Ns\":";
  out += std::to_string(latency.p99_ns);
  out += ',';
  out += "\"p999Ns\":";
  out += std::to_string(latency.p999_ns);
  out += '}';
  out += '}';
  return out;
}

std::expected<BenchmarkResult, std::string> run_benchmark(BenchmarkRunInput input) {
  if (input.options.client_threads == 0) {
    return std::unexpected("client thread count must be greater than zero");
  }
  if (input.options.client_connections == 0) {
    return std::unexpected("client connection count must be greater than zero");
  }
  if (input.connection_openers.size() != input.options.client_connections) {
    return std::unexpected("benchmark transport did not provision the expected connection openers");
  }
  if (auto valid = input.options.message_sizes.validate(); !valid) {
    return std::unexpected(valid.error());
  }
  if (input.options.warmup_seconds < 0.0) {
    return std::unexpected("warmup duration must be zero or greater");
  }
  if (input.options.measure_seconds <= 0.0) {
    return std::unexpected("measure duration must be greater than zero");
  }

  auto thread_configs = split_connection_openers(input);
  std::barrier warmup_barrier(static_cast<std::ptrdiff_t>(input.options.client_threads));
  std::barrier measure_barrier(static_cast<std::ptrdiff_t>(input.options.client_threads));

  std::vector<ThreadResult> results(input.options.client_threads);
  std::vector<std::thread> threads;
  threads.reserve(input.options.client_threads);

  for (std::size_t index = 0; index < input.options.client_threads; ++index) {
    threads.emplace_back([&, index]() {
      results[index] =
          run_client_thread(std::move(thread_configs[index]), warmup_barrier, measure_barrier);
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  for (const auto& thread_result : results) {
    if (thread_result.error) {
      return std::unexpected(*thread_result.error);
    }
  }

  BenchmarkResult result{
      .mode = std::move(input.mode),
      .endpoint = std::move(input.endpoint),
      .client_threads = input.options.client_threads,
      .client_connections = input.options.client_connections,
      .message_sizes = input.options.message_sizes,
      .latency = {},
  };

  double measured_seconds = 0.0;
  std::vector<std::uint64_t> all_latencies;

  for (auto& thread_result : results) {
    result.total_requests += thread_result.stats.total_requests;
    result.errors += thread_result.stats.errors;
    result.request_bytes += thread_result.stats.request_bytes;
    result.response_bytes += thread_result.stats.response_bytes;
    measured_seconds = std::max(measured_seconds, thread_result.stats.measured_seconds);
    all_latencies.insert(all_latencies.end(),
                         std::make_move_iterator(thread_result.stats.latencies_ns.begin()),
                         std::make_move_iterator(thread_result.stats.latencies_ns.end()));
  }

  result.latency = compute_percentiles(std::move(all_latencies));
  result.measured_seconds = measured_seconds;
  result.requests_per_second =
      measured_seconds <= 0.0 ? 0.0 : static_cast<double>(result.total_requests) / measured_seconds;
  result.request_mib_per_second = bytes_to_mib_per_second(result.request_bytes, measured_seconds);
  result.response_mib_per_second = bytes_to_mib_per_second(result.response_bytes, measured_seconds);
  result.combined_mib_per_second =
      bytes_to_mib_per_second(result.request_bytes + result.response_bytes, measured_seconds);

  return result;
}

} // namespace rpcbench
