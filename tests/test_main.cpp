// Minimal test executable for the bootstrap benchmark. The tests prefer direct
// library calls where possible and use one spawn-local integration run to cover
// the end-to-end server and benchmark wiring.

#include "kv.capnp.h"
#include "rpcbench/bench.hpp"
#include "rpcbench/config.hpp"
#include "rpcbench/metrics.hpp"
#include "rpcbench/storage.hpp"

#include <capnp/ez-rpc.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <kj/exception.h>
#include <print>
#include <signal.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
  explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

void expect_contains(std::string_view haystack, std::string_view needle) {
  require(haystack.find(needle) != std::string_view::npos,
          std::format("expected '{}' to contain '{}'", haystack, needle));
}

std::vector<std::string_view> make_args(std::initializer_list<std::string_view> args) {
  return std::vector<std::string_view>(args);
}

std::uint16_t test_port(int slot) {
  return static_cast<std::uint16_t>(47000 + slot * 100 + (::getpid() % 100));
}

struct ChildServer {
  pid_t pid = -1;

  ChildServer() = default;

  explicit ChildServer(pid_t pid_value) : pid(pid_value) {}

  ChildServer(const ChildServer&) = delete;
  ChildServer& operator=(const ChildServer&) = delete;

  ChildServer(ChildServer&& other) noexcept : pid(std::exchange(other.pid, -1)) {}

  ChildServer& operator=(ChildServer&& other) noexcept {
    if (this != &other) {
      stop();
      pid = std::exchange(other.pid, -1);
    }
    return *this;
  }

  ~ChildServer() {
    stop();
  }

  void stop() {
    if (pid <= 0) {
      return;
    }

    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == pid) {
      pid = -1;
      return;
    }

    kill(pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    while (std::chrono::steady_clock::now() < deadline) {
      if (waitpid(pid, &status, WNOHANG) == pid) {
        pid = -1;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    pid = -1;
  }
};

kj::ArrayPtr<const capnp::byte> as_capnp_bytes(const std::string& value) {
  const auto* data = reinterpret_cast<const capnp::byte*>(value.data());
  return kj::ArrayPtr<const capnp::byte>(data, value.size());
}

ChildServer spawn_server_process(const std::filesystem::path& server_path, int port) {
  const auto server_string = server_path.string();
  const pid_t pid = fork();
  require(pid >= 0, "fork should succeed");

  if (pid == 0) {
    const auto port_argument = std::format("--port={}", port);
    execl(server_string.c_str(),
          server_string.c_str(),
          "--listen-host=127.0.0.1",
          port_argument.c_str(),
          "--quiet",
          static_cast<char*>(nullptr));
    _exit(127);
  }

  return ChildServer(pid);
}

void wait_for_server_ready(std::string_view endpoint) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

  while (std::chrono::steady_clock::now() < deadline) {
    try {
      capnp::EzRpcClient client(kj::StringPtr(endpoint.data(), endpoint.size()));
      auto request = client.getMain<KvService>().getRequest();
      request.setKey(as_capnp_bytes(std::string()));
      auto response = request.send().wait(client.getWaitScope());
      static_cast<void>(response);
      return;
    } catch (const kj::Exception&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }

  throw TestFailure(std::format("server did not become ready at {}", endpoint));
}

void test_server_config_parser() {
  auto config = rpcbench::parse_server_config(make_args({
      "--listen-host=0.0.0.0",
      "--port=7100",
      "--quiet",
  }));

  require(config.has_value(), "server config should parse");
  require(config->listen_host == "0.0.0.0", "listen host should match");
  require(config->port == 7100, "port should match");
  require(config->quiet, "quiet flag should be set");
}

void test_bench_config_parser() {
  auto config = rpcbench::parse_bench_config(make_args({
                                                 "--mode=spawn-local",
                                                 "--server-workers=1,2",
                                                 "--client-threads=1,4",
                                                 "--queue-depths=1,8",
                                                 "--key-sizes=8",
                                                 "--value-sizes=32,64",
                                                 "--mixes=80:20:0,50:25:25",
                                                 "--warmup-seconds=0.1",
                                                 "--measure-seconds=0.2",
                                                 "--iterations=2",
                                                 "--key-space=128",
                                                 "--seed=9",
                                                 "--quiet-server",
                                             }),
                                             "/tmp/rpc-bench-bench");

  require(config.has_value(), "benchmark config should parse");
  require(config->mode == rpcbench::BenchMode::spawn_local, "mode should match");
  require(config->server_workers.size() == 2, "worker sweep should parse");
  require(config->workload.client_threads.size() == 2, "client thread sweep should parse");
  require(config->workload.queue_depths.size() == 2, "queue depth sweep should parse");
  require(config->workload.mixes.size() == 2, "mix sweep should parse");
  require(config->workload.iterations == 2, "iteration count should match");
  require(config->quiet_server, "quiet server flag should be set");
}

void test_invalid_parser_inputs() {
  const auto server_config = rpcbench::parse_server_config(make_args({
      "--bogus",
  }));
  require(!server_config.has_value(), "unknown server flag should fail");

  const auto bench_mode = rpcbench::parse_bench_config(make_args({
                                                           "--mode=bogus",
                                                       }),
                                                       "/tmp/rpc-bench-bench");
  require(!bench_mode.has_value(), "unknown benchmark mode should fail");

  const auto bench_connect = rpcbench::parse_bench_config(make_args({
                                                              "--mode=connect",
                                                          }),
                                                          "/tmp/rpc-bench-bench");
  require(!bench_connect.has_value(), "connect mode without endpoints should fail");

  const auto bad_mix = rpcbench::parse_bench_config(make_args({
                                                        "--mode=spawn-local",
                                                        "--mixes=80:10:5",
                                                    }),
                                                    "/tmp/rpc-bench-bench");
  require(!bad_mix.has_value(), "invalid mix totals should fail");

  const auto bad_ports = rpcbench::parse_bench_config(make_args({
                                                          "--mode=spawn-local",
                                                          "--base-port=65535",
                                                          "--server-workers=2",
                                                      }),
                                                      "/tmp/rpc-bench-bench");
  require(!bad_ports.has_value(), "worker port overflow should fail");
}

void test_storage_backend() {
  auto store = rpcbench::make_in_memory_store();
  const std::string key = "key";
  const std::string value = "value";

  const auto initial = store->get(std::as_bytes(std::span(key)));
  require(!initial.found, "missing key should not be found");

  store->put(std::as_bytes(std::span(key)), std::as_bytes(std::span(value)));
  const auto found = store->get(std::as_bytes(std::span(key)));
  require(found.found, "stored key should be found");
  require(found.value == value, "stored value should round-trip");

  require(store->erase(std::as_bytes(std::span(key))), "erase should report removal");
  require(!store->erase(std::as_bytes(std::span(key))), "erasing twice should miss");
}

void test_usage_and_validation_helpers() {
  const auto server_help = rpcbench::server_usage("rpc-bench-server");
  expect_contains(server_help, "--port=PORT");

  const auto bench_help = rpcbench::bench_usage("rpc-bench-bench");
  expect_contains(bench_help, "--mode=connect|spawn-local");
  expect_contains(bench_help, "--json-output=PATH");

  rpcbench::WorkloadSpec invalid_workload;
  invalid_workload.mixes = {rpcbench::OperationMix{
      .get_percent = 10,
      .put_percent = 10,
      .delete_percent = 10,
  }};
  require(!invalid_workload.validate().has_value(), "invalid mix should fail validation");

  rpcbench::BenchConfig invalid_connect;
  invalid_connect.mode = rpcbench::BenchMode::connect;
  require(!invalid_connect.validate().has_value(), "connect mode without endpoints should fail");
}

void test_metrics_rendering() {
  const rpcbench::BenchmarkReport report{
      .mode = rpcbench::BenchMode::spawn_local,
      .results =
          {
              rpcbench::BenchmarkResult{
                  .server_workers = 2,
                  .client_threads = 4,
                  .queue_depth = 8,
                  .key_size = 16,
                  .value_size = 64,
                  .mix =
                      rpcbench::OperationMix{
                          .get_percent = 80,
                          .put_percent = 20,
                          .delete_percent = 0,
                      },
                  .iteration = 1,
                  .measure_seconds = 1.0,
                  .endpoints = {"127.0.0.1:7000", "127.0.0.1:7001"},
                  .counts =
                      rpcbench::OperationCounts{
                          .total_ops = 100,
                          .get_ops = 80,
                          .put_ops = 20,
                          .delete_ops = 0,
                          .found_gets = 75,
                          .missing_gets = 5,
                          .removed_deletes = 0,
                          .missing_deletes = 0,
                          .errors = 0,
                          .request_bytes = 6400,
                          .response_bytes = 4800,
                      },
                  .latency =
                      rpcbench::LatencyPercentiles{
                          .min_ns = 1000,
                          .p50_ns = 2000,
                          .p90_ns = 4000,
                          .p99_ns = 8000,
                          .max_ns = 16000,
                      },
                  .ops_per_second = 100.0,
                  .mib_per_second = 0.010,
              },
          },
  };

  const auto text = report.to_text();
  expect_contains(text, "rpc-bench report");
  expect_contains(text, "workers=2");
  expect_contains(text, "mix=80:20:0");

  const auto json = report.to_json();
  expect_contains(json, "\"mode\": \"spawn-local\"");
  expect_contains(json, "\"clientThreads\": 4");
  expect_contains(json, "\"opsPerSecond\": 100.000000");
}

void test_metrics_edge_rendering() {
  const rpcbench::BenchmarkReport report{
      .mode = rpcbench::BenchMode::connect,
      .results =
          {
              rpcbench::BenchmarkResult{
                  .server_workers = 1,
                  .client_threads = 1,
                  .queue_depth = 1,
                  .key_size = 4,
                  .value_size = 8,
                  .mix =
                      rpcbench::OperationMix{
                          .get_percent = 100,
                          .put_percent = 0,
                          .delete_percent = 0,
                      },
                  .iteration = 1,
                  .measure_seconds = 2.0,
                  .endpoints = {"127.0.0.1:7311", "quoted\"endpoint\n"},
                  .counts =
                      rpcbench::OperationCounts{
                          .total_ops = 1,
                          .get_ops = 1,
                          .put_ops = 0,
                          .delete_ops = 0,
                          .found_gets = 1,
                          .missing_gets = 0,
                          .removed_deletes = 0,
                          .missing_deletes = 0,
                          .errors = 0,
                          .request_bytes = 4,
                          .response_bytes = 8,
                      },
                  .latency =
                      rpcbench::LatencyPercentiles{
                          .min_ns = 500,
                          .p50_ns = 2'000,
                          .p90_ns = 2'000'000,
                          .p99_ns = 2'000'000'000,
                          .max_ns = 2'000'000'000,
                      },
                  .ops_per_second = 0.5,
                  .mib_per_second = 0.001,
              },
          },
  };

  const auto text = report.to_text();
  expect_contains(text, "500ns");
  expect_contains(text, "2.00us");
  expect_contains(text, "2.00ms");
  expect_contains(text, "2.00s");

  const auto json = report.to_json();
  expect_contains(json, "\\\"endpoint\\n");
}

void test_loopback_benchmark() {
  const char* server_path = std::getenv("RPCBENCH_SERVER_PATH"); // NOLINT(concurrency-mt-unsafe)
  require(server_path != nullptr, "RPCBENCH_SERVER_PATH must be set");

  rpcbench::BenchConfig config;
  config.mode = rpcbench::BenchMode::spawn_local;
  config.server_binary = std::filesystem::path(server_path);
  config.server_workers = {1};
  config.listen_host = "127.0.0.1";
  config.base_port = test_port(0);
  config.quiet_server = true;
  config.startup_timeout_ms = 5000;
  config.workload.client_threads = {1};
  config.workload.queue_depths = {2};
  config.workload.key_sizes = {8};
  config.workload.value_sizes = {32};
  config.workload.mixes = {rpcbench::OperationMix{
      .get_percent = 60,
      .put_percent = 40,
      .delete_percent = 0,
  }};
  config.workload.key_space = 32;
  config.workload.warmup_seconds = 0.02;
  config.workload.measure_seconds = 0.05;
  config.workload.iterations = 1;
  config.workload.seed = 7;

  const rpcbench::BenchmarkRunner runner(config);
  auto report = runner.run();
  require(report.has_value(),
          report ? "loopback benchmark should succeed"
                 : std::format("loopback benchmark failed: {}", report.error()));
  require(report->results.size() == 1, "loopback benchmark should produce one result");
  require(report->results.front().counts.total_ops > 0, "loopback benchmark should record work");
  require(report->results.front().counts.errors == 0, "loopback benchmark should not error");
}

void test_connect_mode_loopback() {
  const char* server_path = std::getenv("RPCBENCH_SERVER_PATH"); // NOLINT(concurrency-mt-unsafe)
  require(server_path != nullptr, "RPCBENCH_SERVER_PATH must be set");

  const auto port = test_port(1);
  auto server = spawn_server_process(std::filesystem::path(server_path), port);
  wait_for_server_ready(std::format("127.0.0.1:{}", port));

  rpcbench::BenchConfig config;
  config.mode = rpcbench::BenchMode::connect;
  config.endpoints = {std::format("127.0.0.1:{}", port)};
  config.workload.client_threads = {1};
  config.workload.queue_depths = {1};
  config.workload.key_sizes = {8};
  config.workload.value_sizes = {16};
  config.workload.mixes = {rpcbench::OperationMix{
      .get_percent = 50,
      .put_percent = 50,
      .delete_percent = 0,
  }};
  config.workload.key_space = 32;
  config.workload.warmup_seconds = 0.02;
  config.workload.measure_seconds = 0.05;
  config.workload.iterations = 1;
  config.workload.seed = 11;

  const rpcbench::BenchmarkRunner runner(config);
  auto report = runner.run();
  require(report.has_value(),
          report ? "connect-mode benchmark should succeed"
                 : std::format("connect-mode benchmark failed: {}", report.error()));
  require(report->results.size() == 1, "connect-mode benchmark should produce one result");
  require(report->results.front().counts.total_ops > 0, "connect-mode benchmark should do work");
}

void test_connect_mode_dead_endpoint_fails_fast() {
  rpcbench::BenchConfig config;
  config.mode = rpcbench::BenchMode::connect;
  config.endpoints = {"127.0.0.1:7999"};
  config.startup_timeout_ms = 100;
  config.workload.client_threads = {1};
  config.workload.queue_depths = {1};
  config.workload.key_sizes = {8};
  config.workload.value_sizes = {16};
  config.workload.mixes = {rpcbench::OperationMix{
      .get_percent = 100,
      .put_percent = 0,
      .delete_percent = 0,
  }};
  config.workload.key_space = 8;
  config.workload.warmup_seconds = 0.0;
  config.workload.measure_seconds = 0.01;
  config.workload.iterations = 1;

  const auto start = std::chrono::steady_clock::now();
  const rpcbench::BenchmarkRunner runner(config);
  const auto report = runner.run();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  require(!report.has_value(), "dead endpoint should fail");
  require(elapsed < std::chrono::seconds(2), "dead endpoint failure should be bounded");
}

} // namespace

int main() {
  try {
    test_server_config_parser();
    test_bench_config_parser();
    test_invalid_parser_inputs();
    test_storage_backend();
    test_usage_and_validation_helpers();
    test_metrics_rendering();
    test_metrics_edge_rendering();
    test_loopback_benchmark();
    test_connect_mode_loopback();
    test_connect_mode_dead_endpoint_fails_fast();
  } catch (const TestFailure& failure) {
    std::fprintf(stderr, "test failure: %s\n", failure.what());
    return 1;
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "unexpected failure: %s\n", exception.what());
    return 1;
  }

  return 0;
}
