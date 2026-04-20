// Integration-focused test executable for the CRC32 benchmark runtime. The
// tests cover CLI validation, report rendering, and end-to-end transport
// behavior across tcp://, unix://, and pipe://socketpair.

#include "app/application.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <netdb.h>
#include <optional>
#include <signal.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
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

std::filesystem::path bench_path() {
  const char* value = std::getenv("RPCBENCH_BENCH_PATH");
  require(value != nullptr && value[0] != '\0', "RPCBENCH_BENCH_PATH must be set");
  return std::filesystem::path(value);
}

std::filesystem::path server_path() {
  const char* value = std::getenv("RPCBENCH_SERVER_PATH");
  require(value != nullptr && value[0] != '\0', "RPCBENCH_SERVER_PATH must be set");
  return std::filesystem::path(value);
}

std::uint16_t test_port(int slot) {
  return static_cast<std::uint16_t>(47000 + slot * 100 + (::getpid() % 100));
}

std::filesystem::path test_socket_path(int slot) {
  return std::filesystem::temp_directory_path() /
         std::format("rpc-bench-test-{}-{}.sock", ::getpid(), slot);
}

struct BackgroundProcess {
  pid_t pid = -1;

  BackgroundProcess() = default;
  explicit BackgroundProcess(pid_t pid_value) : pid(pid_value) {}

  BackgroundProcess(const BackgroundProcess&) = delete;
  BackgroundProcess& operator=(const BackgroundProcess&) = delete;

  BackgroundProcess(BackgroundProcess&& other) noexcept : pid(std::exchange(other.pid, -1)) {}

  BackgroundProcess& operator=(BackgroundProcess&& other) noexcept {
    if (this != &other) {
      stop();
      pid = std::exchange(other.pid, -1);
    }
    return *this;
  }

  ~BackgroundProcess() {
    stop();
  }

  void stop() {
    if (pid <= 0) {
      return;
    }

    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      pid = -1;
      return;
    }

    ::kill(pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      if (::waitpid(pid, &status, WNOHANG) == pid) {
        pid = -1;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
    pid = -1;
  }
};

struct CapturedProcess {
  // Captures a child process exit status plus both output streams so CLI
  // integration tests can assert clear user-facing errors and reports.
  int exit_status = -1;
  std::string stdout_text;
  std::string stderr_text;
};

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  require(static_cast<bool>(stream), std::format("expected '{}' to open", path.string()));
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

CapturedProcess run_process_capture(const std::filesystem::path& program,
                                    std::initializer_list<std::string> args) {
  require(std::filesystem::exists(program), "process under test should exist");

  char stdout_template[] = "/tmp/rpc-bench-stdout-XXXXXX";
  char stderr_template[] = "/tmp/rpc-bench-stderr-XXXXXX";
  const int stdout_fd = ::mkstemp(stdout_template);
  const int stderr_fd = ::mkstemp(stderr_template);
  require(stdout_fd >= 0, "stdout capture file should open");
  require(stderr_fd >= 0, "stderr capture file should open");

  const auto program_string = program.string();
  std::vector<std::string> owned_args(args);
  const pid_t pid = ::fork();
  require(pid >= 0, "fork should succeed");

  if (pid == 0) {
    ::dup2(stdout_fd, STDOUT_FILENO);
    ::dup2(stderr_fd, STDERR_FILENO);
    ::close(stdout_fd);
    ::close(stderr_fd);

    std::vector<char*> argv;
    argv.reserve(owned_args.size() + 2);
    argv.push_back(const_cast<char*>(program_string.c_str()));
    for (auto& arg : owned_args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    ::execv(program_string.c_str(), argv.data());
    _exit(127);
  }

  ::close(stdout_fd);
  ::close(stderr_fd);

  int status = 0;
  require(::waitpid(pid, &status, 0) == pid, "waitpid should observe the child");

  const auto stdout_path = std::filesystem::path(stdout_template);
  const auto stderr_path = std::filesystem::path(stderr_template);
  CapturedProcess result{
      .exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1,
      .stdout_text = read_text_file(stdout_path),
      .stderr_text = read_text_file(stderr_path),
  };

  std::filesystem::remove(stdout_path);
  std::filesystem::remove(stderr_path);
  return result;
}

BackgroundProcess spawn_background_process(const std::filesystem::path& program,
                                           std::initializer_list<std::string> args) {
  require(std::filesystem::exists(program), "background process should exist");

  const auto program_string = program.string();
  std::vector<std::string> owned_args(args);
  const pid_t pid = ::fork();
  require(pid >= 0, "fork should succeed");

  if (pid == 0) {
    const int dev_null = ::open("/dev/null", O_WRONLY);
    if (dev_null >= 0) {
      ::dup2(dev_null, STDOUT_FILENO);
      ::dup2(dev_null, STDERR_FILENO);
      ::close(dev_null);
    }

    std::vector<char*> argv;
    argv.reserve(owned_args.size() + 2);
    argv.push_back(const_cast<char*>(program_string.c_str()));
    for (auto& arg : owned_args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    ::execv(program_string.c_str(), argv.data());
    _exit(127);
  }

  return BackgroundProcess(pid);
}

void wait_for_tcp_ready(std::uint16_t port) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  const auto port_text = std::to_string(port);

  while (std::chrono::steady_clock::now() < deadline) {
    ::addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    ::addrinfo* addresses = nullptr;
    const int lookup = ::getaddrinfo("127.0.0.1", port_text.c_str(), &hints, &addresses);
    require(lookup == 0, "getaddrinfo should succeed");

    bool connected = false;
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
      const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (fd < 0) {
        continue;
      }

      if (::connect(fd, address->ai_addr, static_cast<socklen_t>(address->ai_addrlen)) == 0) {
        connected = true;
        ::close(fd);
        break;
      }
      ::close(fd);
    }
    ::freeaddrinfo(addresses);

    if (connected) {
      return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }

  throw TestFailure(std::format("server did not become ready on tcp://127.0.0.1:{}", port));
}

void wait_for_unix_ready(const std::filesystem::path& path) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

  while (std::chrono::steady_clock::now() < deadline) {
    if (std::filesystem::exists(path)) {
      // The Unix socket inode appears only after the server has bound the
      // listener path. Give the listener a moment to settle before the
      // benchmark opens its own connections.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }

  throw TestFailure(std::format("server did not become ready on unix://{}", path.string()));
}

void test_server_config_parser() {
  auto config = rpcbench::parse_server_config(make_args({
      "--listen-uri=unix:///tmp/rpc-bench.sock",
      "--server-threads=2",
      "--quiet",
  }));

  require(config.has_value(), "server config should parse");
  require(config->listen_uri.kind == rpcbench::TransportKind::unix_socket,
          "listen URI kind should match");
  require(config->listen_uri.location == "/tmp/rpc-bench.sock", "listen URI path should match");
  require(config->server_threads == 2, "server thread count should match");
  require(config->quiet, "quiet flag should be set");
}

void test_server_config_pipe_requires_fds() {
  auto config = rpcbench::parse_server_config(make_args({"--listen-uri=pipe://socketpair"}));
  require(!config.has_value(), "pipe server config without fds should fail");
  expect_contains(config.error(), "requires internal preconnected fds");
}

void test_bench_config_parser() {
  auto config = rpcbench::parse_bench_config(make_args({
                                                 "--mode=spawn-local",
                                                 "--listen-uri=pipe://socketpair",
                                                 "--server-threads=1",
                                                 "--client-threads=2",
                                                 "--client-connections=5",
                                                 "--message-size-min=16",
                                                 "--message-size-max=64",
                                                 "--warmup-seconds=0.1",
                                                 "--measure-seconds=0.2",
                                                 "--seed=9",
                                                 "--quiet-server",
                                             }),
                                             "/tmp/rpc-bench-bench");

  require(config.has_value(), "benchmark config should parse");
  require(config->mode == rpcbench::BenchMode::spawn_local, "mode should match");
  require(config->listen_uri.kind == rpcbench::TransportKind::pipe_socketpair,
          "listen URI kind should match");
  require(config->server_threads == 1, "server thread count should match");
  require(config->benchmark.client_threads == 2, "client thread count should match");
  require(config->benchmark.client_connections == 5, "client connection count should match");
  require(config->benchmark.message_sizes.min == 16, "message-size-min should match");
  require(config->benchmark.message_sizes.max == 64, "message-size-max should match");
  require(config->quiet_server, "quiet-server should be set");
}

void test_bench_config_rejects_queue_depths() {
  auto config =
      rpcbench::parse_bench_config(make_args({"--queue-depths=8"}), "/tmp/rpc-bench-bench");
  require(!config.has_value(), "queue-depths should be rejected");
  expect_contains(config.error(), "--client-connections");
}

void test_bench_connect_rejects_server_threads() {
  auto config = rpcbench::parse_bench_config(make_args({
                                                 "--mode=connect",
                                                 "--connect-uri=tcp://127.0.0.1:7000",
                                                 "--server-threads=1",
                                             }),
                                             "/tmp/rpc-bench-bench");
  require(!config.has_value(), "connect mode should reject server-threads");
  expect_contains(config.error(), "does not accept --server-threads");
}

void test_bench_connect_rejects_pipe_uri() {
  auto config = rpcbench::parse_bench_config(
      make_args({"--mode=connect", "--connect-uri=pipe://socketpair"}), "/tmp/rpc-bench-bench");
  require(!config.has_value(), "connect mode should reject pipe URI");
  expect_contains(config.error(), "does not support pipe://socketpair");
}

void test_benchmark_result_formatting() {
  rpcbench::BenchmarkResult result{
      .mode = "spawn-local",
      .endpoint = "pipe://socketpair",
      .client_threads = 2,
      .client_connections = 3,
      .message_sizes =
          rpcbench::MessageSizeRange{
              .min = 16,
              .max = 64,
          },
      .total_requests = 42,
      .errors = 0,
      .request_bytes = 420,
      .response_bytes = 168,
      .measured_seconds = 0.5,
      .requests_per_second = 84.0,
      .request_mib_per_second = 1.0,
      .response_mib_per_second = 0.5,
      .combined_mib_per_second = 1.5,
      .latency =
          rpcbench::LatencyPercentiles{
              .p50_ns = 10,
              .p75_ns = 20,
              .p90_ns = 30,
              .p99_ns = 40,
              .p999_ns = 50,
          },
  };

  const auto text = result.to_text();
  const auto json = result.to_json();
  expect_contains(text, "Client connections: 3");
  expect_contains(text, "Mode: spawn-local");
  expect_contains(json, "\"clientConnections\":3");
  expect_contains(json, "\"mode\":\"spawn-local\"");
}

void test_server_runtime_rejects_multithreaded() {
  const auto process =
      run_process_capture(server_path(),
                          {
                              std::format("--listen-uri=tcp://127.0.0.1:{}", test_port(1)),
                              "--server-threads=2",
                          });
  require(process.exit_status != 0, "server should reject multi-threaded mode");
  expect_contains(process.stderr_text, "--server-threads must be 1");
}

void test_spawn_local_server_rejection_surfaces() {
  const auto process = run_process_capture(bench_path(),
                                           {
                                               "--mode=spawn-local",
                                               "--listen-uri=pipe://socketpair",
                                               "--server-threads=2",
                                               "--client-threads=1",
                                               "--client-connections=1",
                                               "--message-size-min=16",
                                               "--message-size-max=16",
                                               "--warmup-seconds=0.01",
                                               "--measure-seconds=0.02",
                                           });
  require(process.exit_status != 0, "spawn-local run should fail when server rejects threads");
  expect_contains(process.stderr_text, "--server-threads must be 1");
}

void test_connect_mode_tcp() {
  const auto port = test_port(2);
  auto server = spawn_background_process(server_path(),
                                         {
                                             std::format("--listen-uri=tcp://127.0.0.1:{}", port),
                                             "--server-threads=1",
                                             "--quiet",
                                         });
  wait_for_tcp_ready(port);

  const auto process =
      run_process_capture(bench_path(),
                          {
                              "--mode=connect",
                              std::format("--connect-uri=tcp://127.0.0.1:{}", port),
                              "--client-threads=2",
                              "--client-connections=3",
                              "--message-size-min=16",
                              "--message-size-max=32",
                              "--warmup-seconds=0.02",
                              "--measure-seconds=0.05",
                              "--seed=1",
                          });
  require(process.exit_status == 0, "tcp connect benchmark should succeed");
  expect_contains(process.stdout_text, "Mode: connect");
  expect_contains(process.stdout_text, std::format("Endpoint: tcp://127.0.0.1:{}", port));
  expect_contains(process.stdout_text, "Client threads: 2");
  expect_contains(process.stdout_text, "Client connections: 3");
}

void test_connect_mode_unix() {
  const auto path = test_socket_path(3);
  std::filesystem::remove(path);

  auto server = spawn_background_process(server_path(),
                                         {
                                             std::format("--listen-uri=unix://{}", path.string()),
                                             "--server-threads=1",
                                             "--quiet",
                                         });
  wait_for_unix_ready(path);

  const auto process =
      run_process_capture(bench_path(),
                          {
                              "--mode=connect",
                              std::format("--connect-uri=unix://{}", path.string()),
                              "--client-threads=2",
                              "--client-connections=3",
                              "--message-size-min=16",
                              "--message-size-max=32",
                              "--warmup-seconds=0.02",
                              "--measure-seconds=0.05",
                              "--seed=2",
                          });
  require(process.exit_status == 0, "unix connect benchmark should succeed");
  expect_contains(process.stdout_text, "Mode: connect");
  expect_contains(process.stdout_text, std::format("Endpoint: unix://{}", path.string()));
  expect_contains(process.stdout_text, "Client connections: 3");
}

void test_spawn_local_tcp() {
  const auto port = test_port(4);
  const auto process =
      run_process_capture(bench_path(),
                          {
                              "--mode=spawn-local",
                              std::format("--listen-uri=tcp://127.0.0.1:{}", port),
                              std::format("--server-binary={}", server_path().string()),
                              "--server-threads=1",
                              "--client-threads=2",
                              "--client-connections=3",
                              "--message-size-min=16",
                              "--message-size-max=32",
                              "--warmup-seconds=0.02",
                              "--measure-seconds=0.05",
                              "--seed=3",
                              "--quiet-server",
                          });
  require(process.exit_status == 0, "spawn-local tcp benchmark should succeed");
  expect_contains(process.stdout_text, "Mode: spawn-local");
  expect_contains(process.stdout_text, std::format("Endpoint: tcp://127.0.0.1:{}", port));
  expect_contains(process.stdout_text, "Client connections: 3");
}

void test_spawn_local_unix() {
  const auto path = test_socket_path(5);
  std::filesystem::remove(path);

  const auto process =
      run_process_capture(bench_path(),
                          {
                              "--mode=spawn-local",
                              std::format("--listen-uri=unix://{}", path.string()),
                              std::format("--server-binary={}", server_path().string()),
                              "--server-threads=1",
                              "--client-threads=2",
                              "--client-connections=3",
                              "--message-size-min=16",
                              "--message-size-max=32",
                              "--warmup-seconds=0.02",
                              "--measure-seconds=0.05",
                              "--seed=4",
                              "--quiet-server",
                          });
  require(process.exit_status == 0, "spawn-local unix benchmark should succeed");
  expect_contains(process.stdout_text, "Mode: spawn-local");
  expect_contains(process.stdout_text, std::format("Endpoint: unix://{}", path.string()));
  expect_contains(process.stdout_text, "Client connections: 3");
}

void test_spawn_local_pipe() {
  const auto process =
      run_process_capture(bench_path(),
                          {
                              "--mode=spawn-local",
                              "--listen-uri=pipe://socketpair",
                              std::format("--server-binary={}", server_path().string()),
                              "--server-threads=1",
                              "--client-threads=2",
                              "--client-connections=3",
                              "--message-size-min=16",
                              "--message-size-max=32",
                              "--warmup-seconds=0.02",
                              "--measure-seconds=0.05",
                              "--seed=5",
                              "--quiet-server",
                          });
  require(process.exit_status == 0, "spawn-local pipe benchmark should succeed");
  expect_contains(process.stdout_text, "Mode: spawn-local");
  expect_contains(process.stdout_text, "Endpoint: pipe://socketpair");
  expect_contains(process.stdout_text, "Client connections: 3");
}

} // namespace

int main() {
  try {
    test_server_config_parser();
    test_server_config_pipe_requires_fds();
    test_bench_config_parser();
    test_bench_config_rejects_queue_depths();
    test_bench_connect_rejects_server_threads();
    test_bench_connect_rejects_pipe_uri();
    test_benchmark_result_formatting();
    test_server_runtime_rejects_multithreaded();
    test_spawn_local_server_rejection_surfaces();
    test_connect_mode_tcp();
    test_connect_mode_unix();
    test_spawn_local_tcp();
    test_spawn_local_unix();
    test_spawn_local_pipe();
    return 0;
  } catch (const TestFailure& failure) {
    std::fprintf(stderr, "test failure: %s\n", failure.what());
    return 1;
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "unexpected exception: %s\n", exception.what());
    return 1;
  } catch (...) {
    std::fputs("unexpected unknown exception\n", stderr);
    return 1;
  }
}
