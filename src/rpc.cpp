// Cap'n Proto service implementation and the blocking EzRpc server lifecycle.
// The server intentionally owns one shard per process so benchmark worker
// counts map cleanly onto one event loop and one endpoint each.

#include "rpcbench/rpc.hpp"

#include "kv.capnp.h"

#include <capnp/ez-rpc.h>
#include <csignal>
#include <format>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/time.h>
#include <print>
#include <span>
#include <string>

namespace rpcbench {
namespace {

volatile std::sig_atomic_t g_shutdown_requested = 0;

extern "C" void request_shutdown(int) {
  g_shutdown_requested = 1;
}

std::string copy_data(capnp::Data::Reader reader) {
  const auto* data = reinterpret_cast<const char*>(reader.begin());
  return std::string(data, reader.size());
}

std::span<const std::byte> as_bytes(std::string_view bytes) {
  const auto* data = reinterpret_cast<const std::byte*>(bytes.data());
  return std::span<const std::byte>(data, bytes.size());
}

kj::ArrayPtr<const capnp::byte> as_capnp_bytes(const std::string& bytes) {
  const auto* data = reinterpret_cast<const capnp::byte*>(bytes.data());
  return kj::ArrayPtr<const capnp::byte>(data, bytes.size());
}

class KvServiceImpl final : public KvService::Server {
  // Bridges the generated Cap'n Proto service surface to the storage interface.
  // Requests are small and synchronous in this version, so each method returns
  // `READY_NOW` after interacting with the in-memory backend.
public:
  explicit KvServiceImpl(KvStore& store) : store_(store) {}

  kj::Promise<void> get(GetContext context) override {
    const auto params = context.getParams();
    const auto key = copy_data(params.getKey());
    const auto result = store_.get(as_bytes(key));

    auto results = context.getResults();
    results.setFound(result.found);
    results.setValue(as_capnp_bytes(result.value));
    return kj::READY_NOW;
  }

  kj::Promise<void> put(PutContext context) override {
    const auto params = context.getParams();
    const auto key = copy_data(params.getKey());
    const auto value = copy_data(params.getValue());
    store_.put(as_bytes(key), as_bytes(value));
    return kj::READY_NOW;
  }

  kj::Promise<void> delete_(DeleteContext context) override {
    const auto params = context.getParams();
    const auto key = copy_data(params.getKey());

    auto results = context.getResults();
    results.setFound(store_.erase(as_bytes(key)));
    return kj::READY_NOW;
  }

private:
  KvStore& store_;
};

} // namespace

EzRpcServerRunner::EzRpcServerRunner(ServerConfig config, std::shared_ptr<KvStore> store)
    : config_(std::move(config)), store_(std::move(store)) {}

const ServerConfig& EzRpcServerRunner::config() const {
  return config_;
}

void EzRpcServerRunner::run() const {
  g_shutdown_requested = 0;
  const auto previous_sigterm = std::signal(SIGTERM, request_shutdown);
  const auto previous_sigint = std::signal(SIGINT, request_shutdown);

  auto service = kj::heap<KvServiceImpl>(*store_);
  capnp::EzRpcServer server(kj::mv(service), config_.listen_host, config_.port);

  const auto actual_port = server.getPort().wait(server.getWaitScope());
  if (!config_.quiet) {
    std::println("rpc-bench-server listening on {}:{}", config_.listen_host, actual_port);
  }

  while (!g_shutdown_requested) {
    server.getIoProvider()
        .getTimer()
        .afterDelay(100 * kj::MILLISECONDS)
        .wait(server.getWaitScope());
  }

  std::signal(SIGTERM, previous_sigterm);
  std::signal(SIGINT, previous_sigint);
}

} // namespace rpcbench
