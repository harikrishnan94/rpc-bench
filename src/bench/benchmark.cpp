// Benchmark runner for the CRC32 RPC service. The implementation intentionally
// stays closed-loop and single-endpoint so reported latency and throughput map
// directly to the actual request/response path that executed.

#include "bench/benchmark.hpp"

#include "protocol/hash_service.capnp.h"
#include "protocol/shm_transport.hpp"

#include <algorithm>
#include <barrier>
#include <capnp/rpc-twoparty.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <kj/async-io.h>
#include <kj/exception.h>
#include <kj/string.h>
#include <memory>
#include <poll.h>
#include <random>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
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
  // state across sockets.
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

struct SpawnedProcess {
  // Spawn-local mode owns one child server process. The destructor enforces a
  // bounded shutdown so verification runs do not leak background servers.
  pid_t pid = -1;

  SpawnedProcess() = default;
  SpawnedProcess(const SpawnedProcess&) = delete;
  SpawnedProcess& operator=(const SpawnedProcess&) = delete;

  SpawnedProcess(SpawnedProcess&& other) noexcept : pid(std::exchange(other.pid, -1)) {}

  SpawnedProcess& operator=(SpawnedProcess&& other) noexcept {
    if (this != &other) {
      pid = std::exchange(other.pid, -1);
    }
    return *this;
  }

  ~SpawnedProcess() {
    if (pid > 0) {
      int status = 0;
      if (::waitpid(pid, &status, WNOHANG) != pid) {
        ::kill(pid, SIGTERM);
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        while (Clock::now() < deadline) {
          if (::waitpid(pid, &status, WNOHANG) == pid) {
            pid = -1;
            return;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
      }
      pid = -1;
    }
  }
};

struct ReadyPipe {
  // Spawn-local modes use this pipe to learn when the child server finished its
  // transport-specific startup path and is ready for the benchmark parent to
  // proceed.
  kj::AutoCloseFd read_end;
  kj::AutoCloseFd write_end;
};

class ShmBenchmarkTransport;

struct WorkerTransport {
  // Each benchmark worker owns at most one inherited preconnected stream. The
  // network-backed transports leave this empty and connect normally. Shared
  // memory workers borrow a transport context owned by run_benchmark().
  kj::AutoCloseFd preconnected_fd;
  ShmBenchmarkTransport* shm_transport = nullptr;
  std::size_t shm_slot_index = 0;
};

struct SocketpairWorkers {
  // Spawn-local pipe://socketpair mode creates one socketpair per worker before
  // exec so the child inherits the server ends and each benchmark worker keeps
  // its own parent-side stream.
  std::vector<kj::AutoCloseFd> client_fds;
  std::vector<kj::AutoCloseFd> server_fds;
};

// Spawn-local Unix sockets leave a filesystem entry behind, so the benchmark
// owns cleanup on both entry and exit to keep manual smoke runs repeatable.
class UnixSocketPathGuard {
public:
  explicit UnixSocketPathGuard(std::filesystem::path path) : path_(std::move(path)) {
    cleanup();
  }
  UnixSocketPathGuard(const UnixSocketPathGuard&) = delete;
  UnixSocketPathGuard& operator=(const UnixSocketPathGuard&) = delete;
  UnixSocketPathGuard(UnixSocketPathGuard&&) noexcept = default;
  UnixSocketPathGuard& operator=(UnixSocketPathGuard&&) noexcept = default;

  ~UnixSocketPathGuard() {
    cleanup();
  }

private:
  void cleanup() const {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  std::filesystem::path path_;
};

// Each worker owns one explicit TwoPartyVatNetwork + RpcSystem stack so every
// transport, including preconnected streams, flows through the same RPC
// bootstrap path.
class ClientRpcSession {
public:
  explicit ClientRpcSession(kj::Own<kj::AsyncIoStream>&& connection)
      : connection_(kj::mv(connection)), network_(*connection_, capnp::rpc::twoparty::Side::CLIENT),
        rpc_system_(capnp::makeRpcClient(network_)),
        hash_service_(bootstrap_hash_service(rpc_system_)) {}

  explicit ClientRpcSession(kj::Own<capnp::MessageStream>&& message_stream)
      : message_stream_(kj::mv(message_stream)),
        network_(*message_stream_, capnp::rpc::twoparty::Side::CLIENT),
        rpc_system_(capnp::makeRpcClient(network_)),
        hash_service_(bootstrap_hash_service(rpc_system_)) {}

  [[nodiscard]] HashService::Client& hash_service() {
    return hash_service_;
  }

private:
  [[nodiscard]] static HashService::Client
  bootstrap_hash_service(capnp::RpcSystem<capnp::rpc::twoparty::VatId>& rpc_system) {
    capnp::MallocMessageBuilder vat_id_builder;
    auto vat_id = vat_id_builder.initRoot<capnp::rpc::twoparty::VatId>();
    vat_id.setSide(capnp::rpc::twoparty::Side::SERVER);
    return rpc_system.bootstrap(vat_id).castAs<HashService>();
  }

  kj::Own<kj::AsyncIoStream> connection_;
  kj::Own<capnp::MessageStream> message_stream_;
  capnp::TwoPartyVatNetwork network_;
  capnp::RpcSystem<capnp::rpc::twoparty::VatId> rpc_system_;
  HashService::Client hash_service_;
};

struct PayloadGenerator {
  // The generator owns one deterministic stream per thread so runs can be
  // reproduced exactly when the same seed and thread count are used.
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

[[nodiscard]] std::expected<pid_t, std::string>
spawn_process(const std::filesystem::path& program, const std::vector<std::string>& args) {
  const auto program_string = program.string();
  if (!std::filesystem::exists(program)) {
    return std::unexpected(std::format("server binary does not exist: {}", program_string));
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    return std::unexpected(std::format("fork() failed while starting {}", program_string));
  }

  if (pid == 0) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program_string.c_str()));
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(program_string.c_str(), argv.data());
    _exit(127);
  }

  return pid;
}

[[nodiscard]] std::expected<void, std::string> set_fd_inheritable(int fd, bool inheritable) {
  const int current_flags = ::fcntl(fd, F_GETFD);
  if (current_flags < 0) {
    return std::unexpected(std::format("fcntl(F_GETFD) failed for fd {}", fd));
  }

  const int next_flags = inheritable ? (current_flags & ~FD_CLOEXEC) : (current_flags | FD_CLOEXEC);
  if (::fcntl(fd, F_SETFD, next_flags) < 0) {
    return std::unexpected(std::format("fcntl(F_SETFD) failed for fd {}", fd));
  }
  return {};
}

[[nodiscard]] std::expected<ReadyPipe, std::string> create_ready_pipe() {
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    return std::unexpected("pipe() failed while creating the spawn-local ready pipe");
  }

  ReadyPipe pipe{
      .read_end = kj::AutoCloseFd(fds[0]),
      .write_end = kj::AutoCloseFd(fds[1]),
  };
  if (auto cloexec = set_fd_inheritable(pipe.read_end.get(), false); !cloexec) {
    return std::unexpected(cloexec.error());
  }
  if (auto cloexec = set_fd_inheritable(pipe.write_end.get(), false); !cloexec) {
    return std::unexpected(cloexec.error());
  }
  if (auto inheritable = set_fd_inheritable(pipe.write_end.get(), true); !inheritable) {
    return std::unexpected(inheritable.error());
  }
  return pipe;
}

[[nodiscard]] std::expected<void, std::string>
wait_for_ready_pipe(int fd, std::chrono::milliseconds timeout) {
  pollfd descriptor{
      .fd = fd,
      .events = POLLIN,
      .revents = 0,
  };
  const int poll_result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
  if (poll_result < 0) {
    return std::unexpected("poll() failed while waiting for the spawn-local ready pipe");
  }
  if (poll_result == 0) {
    return std::unexpected("spawn-local server did not report readiness before the timeout");
  }

  std::byte ready{};
  const auto bytes_read = ::read(fd, &ready, 1);
  if (bytes_read == 1) {
    return {};
  }
  if (bytes_read == 0) {
    return std::unexpected("spawn-local server exited before reporting readiness");
  }
  return std::unexpected("read() failed while waiting for the spawn-local ready pipe");
}

[[nodiscard]] std::expected<SocketpairWorkers, std::string>
create_socketpair_workers(std::size_t count) {
  SocketpairWorkers workers;
  workers.client_fds.reserve(count);
  workers.server_fds.reserve(count);

  for (std::size_t index = 0; index < count; ++index) {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      return std::unexpected("socketpair() failed while preparing pipe://socketpair workers");
    }

    kj::AutoCloseFd client_end(fds[0]);
    kj::AutoCloseFd server_end(fds[1]);
    if (auto cloexec = set_fd_inheritable(client_end.get(), false); !cloexec) {
      return std::unexpected(cloexec.error());
    }
    if (auto cloexec = set_fd_inheritable(server_end.get(), false); !cloexec) {
      return std::unexpected(cloexec.error());
    }
    if (auto inheritable = set_fd_inheritable(server_end.get(), true); !inheritable) {
      return std::unexpected(inheritable.error());
    }

    workers.server_fds.emplace_back(kj::mv(server_end));
    workers.client_fds.emplace_back(kj::mv(client_end));
  }

  return workers;
}

[[nodiscard]] kj::Own<kj::AsyncIoStream> connect_network_stream(const TransportUri& uri,
                                                                kj::AsyncIoContext& io_context) {
  auto address_text = uri.to_kj_address();
  if (!address_text) {
    KJ_FAIL_REQUIRE("invalid KJ network address", address_text.error().c_str());
  }
  auto address = io_context.provider->getNetwork()
                     .parseAddress(address_text->c_str())
                     .wait(io_context.waitScope);
  return address->connect().wait(io_context.waitScope);
}

[[nodiscard]] std::expected<std::pair<kj::AutoCloseFd, kj::AutoCloseFd>, std::string>
create_control_socketpair() {
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return std::unexpected("socketpair() failed while preparing the shared-memory control socket");
  }

  kj::AutoCloseFd parent_end(fds[0]);
  kj::AutoCloseFd child_end(fds[1]);
  if (auto cloexec = set_fd_inheritable(parent_end.get(), false); !cloexec) {
    return std::unexpected(cloexec.error());
  }
  if (auto cloexec = set_fd_inheritable(child_end.get(), false); !cloexec) {
    return std::unexpected(cloexec.error());
  }
  if (auto inheritable = set_fd_inheritable(child_end.get(), true); !inheritable) {
    return std::unexpected(inheritable.error());
  }
  return std::make_pair(kj::mv(parent_end), kj::mv(child_end));
}

inline constexpr std::size_t kShmRingCapacity = 8;
inline constexpr std::size_t kShmMaxMessageBytes =
    static_cast<std::size_t>(kMaxPayloadSizeBytes) + (static_cast<std::size_t>(64U) * 1024U);

// The benchmark owns one shared-memory region and one parent-side wake broker
// for all shm://NAME workers. The worker threads borrow slot-local views from
// this context while it keeps the mapped region and broker alive for the whole
// run.
class ShmBenchmarkTransport {
public:
  [[nodiscard]] static std::expected<std::unique_ptr<ShmBenchmarkTransport>, std::string>
  create(std::string_view logical_name, std::size_t slot_count, kj::AutoCloseFd control_fd) {
    auto region = ShmTransportRegion::create(logical_name,
                                             ShmTransportOptions{
                                                 .slot_count = slot_count,
                                                 .request_ring_capacity = kShmRingCapacity,
                                                 .response_ring_capacity = kShmRingCapacity,
                                                 .request_max_message_bytes = kShmMaxMessageBytes,
                                                 .response_max_message_bytes = kShmMaxMessageBytes,
                                             });
    if (!region) {
      return std::unexpected(region.error());
    }

    auto transport =
        std::unique_ptr<ShmBenchmarkTransport>(new ShmBenchmarkTransport(kj::mv(*region)));
    transport->wake_listeners_.reserve(slot_count);
    transport->wake_listener_ptrs_.reserve(slot_count);
    for (std::size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
      transport->wake_listeners_.push_back(std::make_unique<CrossThreadWakeListener>());
      transport->wake_listener_ptrs_.push_back(transport->wake_listeners_.back().get());
    }

    transport->broker_ = std::make_unique<ParentWakeBroker>(
        control_fd.release(), std::span<CrossThreadWakeListener*>(transport->wake_listener_ptrs_));
    if (auto started = transport->broker_->start(); !started) {
      return std::unexpected(started.error());
    }

    return transport;
  }

  ShmBenchmarkTransport(const ShmBenchmarkTransport&) = delete;
  ShmBenchmarkTransport& operator=(const ShmBenchmarkTransport&) = delete;
  ShmBenchmarkTransport(ShmBenchmarkTransport&&) = delete;
  ShmBenchmarkTransport& operator=(ShmBenchmarkTransport&&) = delete;
  ~ShmBenchmarkTransport() {
    [[maybe_unused]] const auto cleanup = region_.unlink();
    if (broker_) {
      broker_->stop();
    }
  }

  [[nodiscard]] kj::Own<capnp::MessageStream> make_message_stream(std::size_t slot_index) {
    KJ_REQUIRE(broker_ != nullptr, "shared-memory benchmark transport is missing its wake broker");
    return kj::heap<ShmMessageStream>(slot_index,
                                      region_.benchmark_endpoint(slot_index),
                                      *wake_listeners_.at(slot_index),
                                      *broker_);
  }

  [[nodiscard]] std::expected<void, std::string> send_init_messages() {
    KJ_REQUIRE(broker_ != nullptr, "shared-memory benchmark transport is missing its wake broker");
    for (std::size_t slot_index = 0; slot_index < wake_listeners_.size(); ++slot_index) {
      if (auto sent = broker_->send_init(slot_index); !sent) {
        return std::unexpected(sent.error());
      }
    }
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> unlink_region() const {
    return region_.unlink();
  }

  [[nodiscard]] std::optional<std::string> background_error() const {
    return broker_ ? broker_->background_error() : std::nullopt;
  }

private:
  explicit ShmBenchmarkTransport(ShmTransportRegion&& region) : region_(kj::mv(region)) {}

  ShmTransportRegion region_;
  std::vector<std::unique_ptr<CrossThreadWakeListener>> wake_listeners_;
  std::vector<CrossThreadWakeListener*> wake_listener_ptrs_;
  std::unique_ptr<ParentWakeBroker> broker_;
};

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
                                             WorkerTransport transport,
                                             std::size_t thread_index,
                                             std::barrier<>& warmup_barrier,
                                             std::barrier<>& measure_barrier) {
  bool joined_warmup = false;
  bool joined_measure = false;

  try {
    auto io_context = kj::setupAsyncIo();
    std::optional<ClientRpcSession> rpc_session;
    if (transport.shm_transport != nullptr) {
      rpc_session.emplace(transport.shm_transport->make_message_stream(transport.shm_slot_index));
    } else {
      kj::Own<kj::AsyncIoStream> connection;
      if (transport.preconnected_fd != nullptr) {
        connection = io_context.lowLevelProvider->wrapSocketFd(kj::mv(transport.preconnected_fd));
      } else {
        connection = connect_network_stream(uri, io_context);
      }
      rpc_session.emplace(kj::mv(connection));
    }

    PayloadGenerator generator(thread_index, config.message_sizes, config.seed);
    warmup_barrier.arrive_and_wait();
    joined_warmup = true;
    run_phase(rpc_session->hash_service(),
              generator,
              config.warmup_seconds,
              nullptr,
              io_context.waitScope);

    measure_barrier.arrive_and_wait();
    joined_measure = true;
    ThreadResult result;
    run_phase(rpc_session->hash_service(),
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

} // namespace

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
    if (connect_uri->kind == TransportKind::pipe_socketpair ||
        connect_uri->kind == TransportKind::shared_memory) {
      return std::unexpected("connect mode only supports tcp://... and unix://...");
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
  std::unique_ptr<ShmBenchmarkTransport> shm_transport;
  SpawnedProcess spawned_server;
  std::vector<WorkerTransport> worker_transports(config.client_threads);
  std::optional<UnixSocketPathGuard> unix_socket_guard;
  if (config.mode == BenchMode::spawn_local) {
    auto ready_pipe = create_ready_pipe();
    if (!ready_pipe) {
      return std::unexpected(ready_pipe.error());
    }

    std::vector<std::string> args{
        std::format("--listen-uri={}", config.listen_uri.to_string()),
        std::format("--internal-ready-fd={}", ready_pipe->write_end.get()),
    };
    if (config.quiet_server) {
      args.emplace_back("--quiet");
    }

    std::optional<SocketpairWorkers> socketpair_workers;
    std::optional<std::pair<kj::AutoCloseFd, kj::AutoCloseFd>> shm_control_socketpair;
    switch (config.listen_uri.kind) {
    case TransportKind::tcp:
      break;
    case TransportKind::unix_socket:
      unix_socket_guard.emplace(config.listen_uri.location);
      break;
    case TransportKind::pipe_socketpair: {
      auto workers = create_socketpair_workers(config.client_threads);
      if (!workers) {
        return std::unexpected(workers.error());
      }
      socketpair_workers = std::move(*workers);

      std::string joined;
      for (std::size_t index = 0; index < socketpair_workers->server_fds.size(); ++index) {
        if (index > 0) {
          joined.push_back(',');
        }
        joined += std::to_string(socketpair_workers->server_fds[index].get());
      }
      args.push_back(std::format("--internal-preconnected-fds={}", joined));
      break;
    }
    case TransportKind::shared_memory: {
      auto control_socketpair = create_control_socketpair();
      if (!control_socketpair) {
        return std::unexpected(control_socketpair.error());
      }
      shm_control_socketpair = std::move(*control_socketpair);

      auto created_transport = ShmBenchmarkTransport::create(
          config.listen_uri.location, config.client_threads, kj::mv(shm_control_socketpair->first));
      if (!created_transport) {
        return std::unexpected(created_transport.error());
      }
      shm_transport = std::move(*created_transport);

      args.push_back(
          std::format("--internal-shm-control-fd={}", shm_control_socketpair->second.get()));
      args.push_back(std::format("--internal-shm-slot-count={}", config.client_threads));
      break;
    }
    case TransportKind::unspecified:
      return std::unexpected("listen URI is not configured");
    }

    auto pid = spawn_process(std::filesystem::absolute(config.server_binary), args);
    if (!pid) {
      return std::unexpected(pid.error());
    }
    spawned_server.pid = *pid;
    ready_pipe->write_end = nullptr;

    if (socketpair_workers) {
      for (std::size_t index = 0; index < config.client_threads; ++index) {
        worker_transports[index].preconnected_fd = kj::mv(socketpair_workers->client_fds[index]);
      }
    }
    if (shm_transport) {
      if (auto sent = shm_transport->send_init_messages(); !sent) {
        return std::unexpected(sent.error());
      }
    }

    auto ready = wait_for_ready_pipe(ready_pipe->read_end.get(),
                                     std::chrono::milliseconds(config.startup_timeout_ms));
    if (!ready) {
      return std::unexpected(ready.error());
    }
    if (shm_transport) {
      if (auto unlinked = shm_transport->unlink_region(); !unlinked) {
        return std::unexpected(unlinked.error());
      }
      for (std::size_t index = 0; index < config.client_threads; ++index) {
        worker_transports[index].shm_transport = shm_transport.get();
        worker_transports[index].shm_slot_index = index;
      }
    }
  }

  const auto uri = config.resolved_uri();
  std::barrier<> warmup_barrier(static_cast<std::ptrdiff_t>(config.client_threads + 1));
  std::barrier<> measure_barrier(static_cast<std::ptrdiff_t>(config.client_threads + 1));

  std::vector<ThreadResult> results(config.client_threads);
  std::vector<std::thread> threads;
  threads.reserve(config.client_threads);

  for (std::size_t index = 0; index < config.client_threads; ++index) {
    threads.emplace_back([&, index, transport = kj::mv(worker_transports[index])]() mutable {
      results[index] =
          run_client_thread(uri, config, kj::mv(transport), index, warmup_barrier, measure_barrier);
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
  if (shm_transport) {
    if (const auto background_error = shm_transport->background_error(); background_error) {
      return std::unexpected(
          std::format("shared-memory wake broker failed: {}", *background_error));
    }
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
