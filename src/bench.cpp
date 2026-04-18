// Benchmark execution engine for local-spawn and remote modes. The core design
// keeps each EzRpc client thread-affine, expands one explicit sweep matrix, and
// treats server worker counts as endpoint counts so measurements remain easy to
// reproduce locally and remotely.

#include "rpcbench/bench.hpp"

#include "kv.capnp.h"

#include <algorithm>
#include <array>
#include <capnp/ez-rpc.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <kj/async.h>
#include <kj/exception.h>
#include <limits>
#include <netdb.h>
#include <optional>
#include <poll.h>
#include <random>
#include <signal.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace rpcbench {
namespace {

using Clock = std::chrono::steady_clock;

enum class OperationKind : std::uint8_t {
  get,
  put,
  delete_,
};

struct OperationPlan {
  OperationKind kind;
  std::string key;
  std::string value;
};

struct RunCase {
  std::size_t server_workers = 0;
  std::size_t client_threads = 0;
  std::size_t queue_depth = 0;
  std::size_t key_size = 0;
  std::size_t value_size = 0;
  OperationMix mix;
  std::uint32_t iteration = 0;
};

struct ThreadStats {
  OperationCounts counts;
  std::vector<std::uint64_t> latencies_ns;
};

struct ThreadResult {
  ThreadStats stats;
  std::optional<std::string> error;
};

struct SpawnedProcess {
  pid_t pid = -1;
  std::string endpoint;
};

kj::ArrayPtr<const capnp::byte> as_capnp_bytes(const std::string& value) {
  const auto* data = reinterpret_cast<const capnp::byte*>(value.data());
  return kj::ArrayPtr<const capnp::byte>(data, value.size());
}

std::string kj_exception_message(const kj::Exception& exception) {
  return std::format(
      "{}:{} {}", exception.getFile(), exception.getLine(), exception.getDescription().cStr());
}

std::string make_endpoint(std::string_view host, std::uint16_t port) {
  return std::format("{}:{}", host, port);
}

std::uint64_t mix_seed(const OperationMix& mix) {
  return (static_cast<std::uint64_t>(mix.get_percent) << 32U) ^
         (static_cast<std::uint64_t>(mix.put_percent) << 16U) ^
         static_cast<std::uint64_t>(mix.delete_percent);
}

std::uint64_t derive_seed(const RunCase& run, std::size_t thread_index, std::uint64_t seed) {
  return seed ^ (run.server_workers * 0x9e3779b185ebca87ULL) ^
         (run.client_threads * 0xc2b2ae3d27d4eb4fULL) ^ (run.queue_depth * 0x165667b19e3779f9ULL) ^
         (run.key_size * 0x85ebca77c2b2ae63ULL) ^ (run.value_size * 0x27d4eb2f165667c5ULL) ^
         (mix_seed(run.mix) * 0x94d049bb133111ebULL) ^
         (static_cast<std::uint64_t>(run.iteration) * 0xd6e8feb86659fd93ULL) ^
         (thread_index * 0xa24baed4963ee407ULL);
}

std::expected<pid_t, std::string> spawn_process(const std::filesystem::path& program,
                                                const std::vector<std::string>& arguments,
                                                bool quiet) {
  const auto program_string = program.string();
  if (!std::filesystem::exists(program)) {
    return std::unexpected(std::format("server binary does not exist: {}", program_string));
  }

  const pid_t pid = fork();
  if (pid < 0) {
    return std::unexpected(std::format("fork() failed while starting {}", program_string));
  }

  if (pid == 0) {
    if (quiet) {
      const int dev_null = open("/dev/null", O_WRONLY);
      if (dev_null >= 0) {
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);
        close(dev_null);
      }
    }

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(program_string.c_str()));
    for (const auto& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    execv(program_string.c_str(), argv.data());
    _exit(127);
  }

  return pid;
}

void stop_process(const SpawnedProcess& process) {
  if (process.pid <= 0) {
    return;
  }

  int status = 0;
  if (waitpid(process.pid, &status, WNOHANG) == process.pid) {
    return;
  }

  kill(process.pid, SIGTERM);
  const auto deadline = Clock::now() + std::chrono::seconds(2);

  while (Clock::now() < deadline) {
    const auto waited = waitpid(process.pid, &status, WNOHANG);
    if (waited == process.pid) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }

  kill(process.pid, SIGKILL);
  waitpid(process.pid, &status, 0);
}

std::expected<void, std::string> wait_for_endpoint_ready(std::string_view endpoint,
                                                         std::chrono::milliseconds timeout) {
  auto split_endpoint = [](std::string_view value)
      -> std::expected<std::pair<std::string, std::string>, std::string> {
    if (value.starts_with('[')) {
      const auto close = value.rfind(']');
      if (close == std::string_view::npos || close + 2 >= value.size() || value[close + 1] != ':') {
        return std::unexpected(std::format("invalid endpoint '{}'", value));
      }
      return std::pair<std::string, std::string>{
          std::string(value.substr(1, close - 1)),
          std::string(value.substr(close + 2)),
      };
    }

    const auto colon = value.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= value.size()) {
      return std::unexpected(std::format("invalid endpoint '{}'", value));
    }

    return std::pair<std::string, std::string>{
        std::string(value.substr(0, colon)),
        std::string(value.substr(colon + 1)),
    };
  };

  auto parts = split_endpoint(endpoint);
  if (!parts) {
    return std::unexpected(parts.error());
  }

  ::addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  ::addrinfo* raw_addresses = nullptr;
  const auto lookup_result =
      ::getaddrinfo(parts->first.c_str(), parts->second.c_str(), &hints, &raw_addresses);
  if (lookup_result != 0) {
    return std::unexpected(std::format(
        "could not resolve endpoint '{}': {}", endpoint, ::gai_strerror(lookup_result)));
  }

  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    for (auto* address = raw_addresses; address != nullptr; address = address->ai_next) {
      const int socket_fd =
          ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (socket_fd < 0) {
        continue;
      }

      const auto original_flags = ::fcntl(socket_fd, F_GETFL, 0);
      if (original_flags >= 0) {
        static_cast<void>(::fcntl(socket_fd, F_SETFL, original_flags | O_NONBLOCK));
      }

      const int connect_result =
          ::connect(socket_fd, address->ai_addr, static_cast<socklen_t>(address->ai_addrlen));
      if (connect_result == 0) {
        ::close(socket_fd);
        ::freeaddrinfo(raw_addresses);
        return {};
      }

      if (connect_result < 0 && errno == EINPROGRESS) {
        const auto now = Clock::now();
        if (now >= deadline) {
          ::close(socket_fd);
          break;
        }

        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        ::pollfd descriptor{
            .fd = socket_fd,
            .events = POLLOUT,
            .revents = 0,
        };

        if (::poll(&descriptor, 1, static_cast<int>(remaining.count())) > 0) {
          int socket_error = 0;
          socklen_t option_length = sizeof(socket_error);
          if (::getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &option_length) == 0 &&
              socket_error == 0) {
            ::close(socket_fd);
            ::freeaddrinfo(raw_addresses);
            return {};
          }
        }
      }

      ::close(socket_fd);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }

  ::freeaddrinfo(raw_addresses);
  return std::unexpected(std::format("endpoint did not become ready: {}", endpoint));
}

class SpawnedServerCluster {
  // Owns the local server processes for one worker-count matrix slice. The
  // benchmark runner keeps the cluster alive across client-side sweeps so each
  // run can reset state with a prefill pass instead of a full process restart.
public:
  SpawnedServerCluster() = default;
  SpawnedServerCluster(const SpawnedServerCluster&) = delete;
  SpawnedServerCluster& operator=(const SpawnedServerCluster&) = delete;

  SpawnedServerCluster(SpawnedServerCluster&&) = default;
  SpawnedServerCluster& operator=(SpawnedServerCluster&&) = default;

  ~SpawnedServerCluster() {
    for (const auto& process : processes_) {
      stop_process(process);
    }
  }

  static std::expected<SpawnedServerCluster, std::string> spawn(const BenchConfig& config,
                                                                std::size_t worker_count) {
    SpawnedServerCluster cluster;
    const auto program = std::filesystem::absolute(config.server_binary);

    for (std::size_t index = 0; index < worker_count; ++index) {
      const auto port = static_cast<std::uint16_t>(config.base_port + index);
      const auto endpoint = make_endpoint(config.listen_host, port);
      std::vector<std::string> arguments{
          std::format("--listen-host={}", config.listen_host),
          std::format("--port={}", port),
      };
      if (config.quiet_server) {
        arguments.emplace_back("--quiet");
      }

      auto pid = spawn_process(program, arguments, config.quiet_server);
      if (!pid) {
        return std::unexpected(pid.error());
      }

      cluster.processes_.push_back(SpawnedProcess{
          .pid = *pid,
          .endpoint = endpoint,
      });
      cluster.endpoints_.push_back(endpoint);
    }

    for (const auto& endpoint : cluster.endpoints_) {
      auto ready =
          wait_for_endpoint_ready(endpoint, std::chrono::milliseconds(config.startup_timeout_ms));
      if (!ready) {
        return std::unexpected(ready.error());
      }
    }

    return cluster;
  }

  [[nodiscard]] const std::vector<std::string>& endpoints() const {
    return endpoints_;
  }

private:
  std::vector<SpawnedProcess> processes_;
  std::vector<std::string> endpoints_;
};

class WorkloadGenerator {
  // Produces deterministic thread-local keys, values, and operation choices for
  // one benchmark thread. Each thread owns its own logical key-space so a
  // thread can remain bound to one endpoint without cross-thread client usage.
public:
  struct Config {
    std::size_t thread_index = 0;
    std::size_t key_space = 0;
    std::uint64_t seed = 0;
  };

  WorkloadGenerator(const RunCase& run, Config config)
      : run_(run), thread_index_(config.thread_index), key_space_(config.key_space),
        seed_(config.seed), rng_(config.seed), key_distribution_(0, config.key_space - 1),
        mix_distribution_(0, 99) {}

  [[nodiscard]] std::size_t key_space() const {
    return key_space_;
  }

  [[nodiscard]] std::string make_key(std::size_t slot) const {
    std::string key(run_.key_size, '\0');
    fill_bytes(key, slot ^ 0x6a09e667f3bcc909ULL);
    return key;
  }

  [[nodiscard]] std::string make_value(std::size_t slot, std::uint64_t version) const {
    std::string value(run_.value_size, '\0');
    fill_bytes(value, slot ^ version ^ 0xbb67ae8584caa73bULL);
    return value;
  }

  [[nodiscard]] OperationPlan next() {
    const auto slot = key_distribution_(rng_);
    const auto pick = mix_distribution_(rng_);

    OperationKind kind = OperationKind::put;
    if (pick < static_cast<int>(run_.mix.get_percent)) {
      kind = OperationKind::get;
    } else if (pick < static_cast<int>(run_.mix.get_percent + run_.mix.put_percent)) {
      kind = OperationKind::put;
    } else {
      kind = OperationKind::delete_;
    }

    OperationPlan plan{
        .kind = kind,
        .key = make_key(slot),
        .value = {},
    };

    if (kind == OperationKind::put) {
      plan.value = make_value(slot, sequence_++);
    }

    return plan;
  }

private:
  void fill_bytes(std::string& bytes, std::uint64_t selector) const {
    auto state =
        seed_ ^ (thread_index_ * 0x9e3779b185ebca87ULL) ^ (selector * 0xc2b2ae3d27d4eb4fULL);

    for (std::size_t index = 0; index < bytes.size(); ++index) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      bytes[index] = static_cast<char>(state >> 56U);
    }
  }

  RunCase run_;
  std::size_t thread_index_;
  std::size_t key_space_;
  std::uint64_t seed_;
  std::mt19937_64 rng_;
  std::uniform_int_distribution<std::size_t> key_distribution_;
  std::uniform_int_distribution<int> mix_distribution_;
  std::uint64_t sequence_ = 1;
};

class ThreadSession {
  // Owns one benchmark thread's EzRpc client, workload generator, and bounded
  // in-flight request chains. All asynchronous callbacks run on the same event
  // loop, which keeps EzRpc usage aligned with its thread-affinity rules.
public:
  ThreadSession(std::string endpoint,
                const RunCase& run,
                const WorkloadSpec& workload,
                std::size_t thread_index)
      : endpoint_(std::move(endpoint)), run_(run), workload_(workload),
        generator_(run,
                   WorkloadGenerator::Config{
                       .thread_index = thread_index,
                       .key_space = workload.key_space,
                       .seed = derive_seed(run, thread_index, workload.seed),
                   }),
        client_(endpoint_), service_(client_.getMain<KvService>()),
        wait_scope_(client_.getWaitScope()) {}

  [[nodiscard]] std::expected<ThreadStats, std::string> execute() {
    try {
      prefill();
      if (workload_.warmup_seconds > 0.0) {
        auto warmup = execute_phase(workload_.warmup_seconds, false);
        if (!warmup) {
          return std::unexpected(warmup.error());
        }
      }

      return execute_phase(workload_.measure_seconds, true);
    } catch (const kj::Exception& exception) {
      return std::unexpected(kj_exception_message(exception));
    }
  }

private:
  void prefill() {
    // Prefill keeps each measured iteration comparable even when endpoints are
    // reused across multiple matrix points or remote runs.
    for (std::size_t slot = 0; slot < generator_.key_space(); ++slot) {
      auto request = service_.putRequest();
      const auto key = generator_.make_key(slot);
      const auto value = generator_.make_value(slot, 0);
      request.setKey(as_capnp_bytes(key));
      request.setValue(as_capnp_bytes(value));
      request.send().wait(wait_scope_);
    }
  }

  [[nodiscard]] std::expected<ThreadStats, std::string> execute_phase(double seconds,
                                                                      bool record_metrics) {
    ThreadStats stats;
    stats_ = &stats;
    record_metrics_ = record_metrics;
    fatal_error_.reset();

    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                             std::chrono::duration<double>(seconds));
    kj::Vector<kj::Promise<void>> tasks;
    tasks.reserve(run_.queue_depth);

    for (std::size_t chain = 0; chain < run_.queue_depth; ++chain) {
      tasks.add(issue_loop(chain, deadline));
    }

    kj::joinPromises(tasks.releaseAsArray()).wait(wait_scope_);
    stats_ = nullptr;

    if (fatal_error_) {
      return std::unexpected(*fatal_error_);
    }

    return stats;
  }

  kj::Promise<void> issue_loop(std::size_t chain_index, Clock::time_point deadline) {
    if (fatal_error_ || Clock::now() >= deadline) {
      return kj::READY_NOW;
    }

    auto plan = generator_.next();
    const auto started = Clock::now();

    switch (plan.kind) {
    case OperationKind::get: {
      auto request = service_.getRequest();
      const auto key_size = plan.key.size();
      request.setKey(as_capnp_bytes(plan.key));
      return request.send()
          .then([this, started, deadline, chain_index, key_size](
                    capnp::Response<KvService::GetResults>&& response) -> kj::Promise<void> {
            if (record_metrics_) {
              auto& counts = stats_->counts;
              counts.total_ops += 1;
              counts.get_ops += 1;
              counts.request_bytes += key_size;

              if (response.getFound()) {
                counts.found_gets += 1;
                counts.response_bytes += response.getValue().size();
                if (response.getValue().size() != run_.value_size && !fatal_error_) {
                  fatal_error_ = std::format("endpoint {} returned value size {} instead of {}",
                                             endpoint_,
                                             response.getValue().size(),
                                             run_.value_size);
                  counts.errors += 1;
                  return kj::READY_NOW;
                }
              } else {
                counts.missing_gets += 1;
              }

              stats_->latencies_ns.push_back(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started)
                      .count());
            }

            return issue_loop(chain_index, deadline);
          })
          .catch_([this](kj::Exception&& exception) -> kj::Promise<void> {
            if (!fatal_error_) {
              fatal_error_ = kj_exception_message(exception);
              if (record_metrics_) {
                stats_->counts.errors += 1;
              }
            }
            return kj::READY_NOW;
          });
    }
    case OperationKind::put: {
      auto request = service_.putRequest();
      const auto key_size = plan.key.size();
      const auto value_size = plan.value.size();
      request.setKey(as_capnp_bytes(plan.key));
      request.setValue(as_capnp_bytes(plan.value));
      return request.send()
          .then([this, started, deadline, chain_index, key_size, value_size](
                    capnp::Response<KvService::PutResults>&&) -> kj::Promise<void> {
            if (record_metrics_) {
              auto& counts = stats_->counts;
              counts.total_ops += 1;
              counts.put_ops += 1;
              counts.request_bytes += key_size + value_size;
              stats_->latencies_ns.push_back(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started)
                      .count());
            }

            return issue_loop(chain_index, deadline);
          })
          .catch_([this](kj::Exception&& exception) -> kj::Promise<void> {
            if (!fatal_error_) {
              fatal_error_ = kj_exception_message(exception);
              if (record_metrics_) {
                stats_->counts.errors += 1;
              }
            }
            return kj::READY_NOW;
          });
    }
    case OperationKind::delete_: {
      auto request = service_.deleteRequest();
      const auto key_size = plan.key.size();
      request.setKey(as_capnp_bytes(plan.key));
      return request.send()
          .then([this, started, deadline, chain_index, key_size](
                    capnp::Response<KvService::DeleteResults>&& response) -> kj::Promise<void> {
            if (record_metrics_) {
              auto& counts = stats_->counts;
              counts.total_ops += 1;
              counts.delete_ops += 1;
              counts.request_bytes += key_size;
              if (response.getFound()) {
                counts.removed_deletes += 1;
              } else {
                counts.missing_deletes += 1;
              }
              stats_->latencies_ns.push_back(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started)
                      .count());
            }

            return issue_loop(chain_index, deadline);
          })
          .catch_([this](kj::Exception&& exception) -> kj::Promise<void> {
            if (!fatal_error_) {
              fatal_error_ = kj_exception_message(exception);
              if (record_metrics_) {
                stats_->counts.errors += 1;
              }
            }
            return kj::READY_NOW;
          });
    }
    }

    return kj::READY_NOW;
  }

  std::string endpoint_;
  RunCase run_;
  WorkloadSpec workload_;
  WorkloadGenerator generator_;
  capnp::EzRpcClient client_;
  KvService::Client service_;
  kj::WaitScope& wait_scope_;
  ThreadStats* stats_ = nullptr;
  bool record_metrics_ = false;
  std::optional<std::string> fatal_error_;
};

LatencyPercentiles compute_percentiles(std::vector<std::uint64_t> latencies) {
  if (latencies.empty()) {
    return {};
  }

  std::ranges::sort(latencies);
  const auto percentile = [&](double fraction) {
    const auto index =
        static_cast<std::size_t>(std::floor(fraction * static_cast<double>(latencies.size() - 1)));
    return latencies[index];
  };

  return LatencyPercentiles{
      .min_ns = latencies.front(),
      .p50_ns = percentile(0.50),
      .p90_ns = percentile(0.90),
      .p99_ns = percentile(0.99),
      .max_ns = latencies.back(),
  };
}

void merge_counts(OperationCounts& target, const OperationCounts& source) {
  target.total_ops += source.total_ops;
  target.get_ops += source.get_ops;
  target.put_ops += source.put_ops;
  target.delete_ops += source.delete_ops;
  target.found_gets += source.found_gets;
  target.missing_gets += source.missing_gets;
  target.removed_deletes += source.removed_deletes;
  target.missing_deletes += source.missing_deletes;
  target.errors += source.errors;
  target.request_bytes += source.request_bytes;
  target.response_bytes += source.response_bytes;
}

std::vector<RunCase> build_cases(std::size_t server_workers, const WorkloadSpec& workload) {
  std::vector<RunCase> cases;

  for (const auto client_threads : workload.client_threads) {
    for (const auto queue_depth : workload.queue_depths) {
      for (const auto key_size : workload.key_sizes) {
        for (const auto value_size : workload.value_sizes) {
          for (const auto& mix : workload.mixes) {
            for (std::uint32_t iteration = 1; iteration <= workload.iterations; ++iteration) {
              cases.push_back(RunCase{
                  .server_workers = server_workers,
                  .client_threads = client_threads,
                  .queue_depth = queue_depth,
                  .key_size = key_size,
                  .value_size = value_size,
                  .mix = mix,
                  .iteration = iteration,
              });
            }
          }
        }
      }
    }
  }

  return cases;
}

std::expected<BenchmarkResult, std::string> run_case(const std::vector<std::string>& endpoints,
                                                     const RunCase& run,
                                                     const WorkloadSpec& workload) {
  std::vector<ThreadResult> thread_results(run.client_threads);
  std::vector<std::thread> threads;
  threads.reserve(run.client_threads);

  for (std::size_t thread_index = 0; thread_index < run.client_threads; ++thread_index) {
    threads.emplace_back([&, thread_index] {
      ThreadSession session(
          endpoints[thread_index % endpoints.size()], run, workload, thread_index);
      auto result = session.execute();
      if (!result) {
        thread_results[thread_index].error = result.error();
        return;
      }
      thread_results[thread_index].stats = std::move(*result);
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  std::vector<std::string> errors;
  OperationCounts counts;
  std::vector<std::uint64_t> latencies;

  for (const auto& thread_result : thread_results) {
    if (thread_result.error) {
      errors.push_back(*thread_result.error);
      continue;
    }

    merge_counts(counts, thread_result.stats.counts);
    latencies.insert(latencies.end(),
                     thread_result.stats.latencies_ns.begin(),
                     thread_result.stats.latencies_ns.end());
  }

  if (!errors.empty()) {
    return std::unexpected(std::format("benchmark thread failed: {}", errors.front()));
  }

  const auto measured_bytes = static_cast<double>(counts.request_bytes + counts.response_bytes);
  const auto ops_per_second = static_cast<double>(counts.total_ops) / workload.measure_seconds;
  const auto mib_per_second = measured_bytes / workload.measure_seconds / (1024.0 * 1024.0);

  return BenchmarkResult{
      .server_workers = run.server_workers,
      .client_threads = run.client_threads,
      .queue_depth = run.queue_depth,
      .key_size = run.key_size,
      .value_size = run.value_size,
      .mix = run.mix,
      .iteration = run.iteration,
      .measure_seconds = workload.measure_seconds,
      .endpoints = endpoints,
      .counts = counts,
      .latency = compute_percentiles(std::move(latencies)),
      .ops_per_second = ops_per_second,
      .mib_per_second = mib_per_second,
  };
}

} // namespace

BenchmarkRunner::BenchmarkRunner(BenchConfig config) : config_(std::move(config)) {}

std::expected<BenchmarkReport, std::string> BenchmarkRunner::run() const {
  if (auto valid = config_.validate(); !valid) {
    return std::unexpected(valid.error());
  }

  BenchmarkReport report;
  report.mode = config_.mode;

  if (config_.mode == BenchMode::connect) {
    for (const auto& endpoint : config_.endpoints) {
      auto ready =
          wait_for_endpoint_ready(endpoint, std::chrono::milliseconds(config_.startup_timeout_ms));
      if (!ready) {
        return std::unexpected(ready.error());
      }
    }

    const auto cases = build_cases(config_.endpoints.size(), config_.workload);
    for (const auto& run : cases) {
      auto result = run_case(config_.endpoints, run, config_.workload);
      if (!result) {
        return std::unexpected(result.error());
      }
      report.results.push_back(std::move(*result));
    }
    return report;
  }

  for (const auto worker_count : config_.server_workers) {
    auto cluster = SpawnedServerCluster::spawn(config_, worker_count);
    if (!cluster) {
      return std::unexpected(cluster.error());
    }

    const auto cases = build_cases(worker_count, config_.workload);
    for (const auto& run : cases) {
      auto result = run_case(cluster->endpoints(), run, config_.workload);
      if (!result) {
        return std::unexpected(result.error());
      }
      report.results.push_back(std::move(*result));
    }
  }

  return report;
}

} // namespace rpcbench
