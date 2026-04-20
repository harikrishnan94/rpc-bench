#pragma once

// Shared stream-backed RPC session wrappers. These classes hide the Cap'n
// Proto two-party bootstrap details so the benchmark and server runtime can
// stay focused on async stream ownership and coroutine flow.

#include "protocol/hash_service.capnp.h"

#include <capnp/rpc-twoparty.h>
#include <kj/async-io.h>
#include <kj/memory.h>
#include <optional>

namespace rpcbench {

class ClientRpcSession {
  // Owns one client-side two-party RPC stack for the lifetime of one benchmark
  // worker connection.
public:
  explicit ClientRpcSession(kj::Own<kj::AsyncIoStream>&& connection);
  ClientRpcSession(const ClientRpcSession&) = delete;
  ClientRpcSession& operator=(const ClientRpcSession&) = delete;
  ClientRpcSession(ClientRpcSession&& other) noexcept = delete;
  ClientRpcSession& operator=(ClientRpcSession&& other) noexcept = delete;

  [[nodiscard]] HashService::Client& hash_service();

private:
  [[nodiscard]] static HashService::Client
  bootstrap_hash_service(capnp::RpcSystem<capnp::rpc::twoparty::VatId>& rpc_system);

  kj::Own<kj::AsyncIoStream> connection_;
  std::optional<capnp::TwoPartyVatNetwork> network_;
  std::optional<capnp::RpcSystem<capnp::rpc::twoparty::VatId>> rpc_system_;
  std::optional<HashService::Client> hash_service_;
};

class ServerRpcSession {
  // Owns one server-side two-party RPC stack for the lifetime of one accepted
  // benchmark session.
public:
  ServerRpcSession(capnp::Capability::Client bootstrap, kj::Own<kj::AsyncIoStream>&& connection);
  ServerRpcSession(const ServerRpcSession&) = delete;
  ServerRpcSession& operator=(const ServerRpcSession&) = delete;
  ServerRpcSession(ServerRpcSession&& other) noexcept = delete;
  ServerRpcSession& operator=(ServerRpcSession&& other) noexcept = delete;

  [[nodiscard]] kj::Promise<void> run();

private:
  kj::Own<kj::AsyncIoStream> connection_;
  std::optional<capnp::TwoPartyVatNetwork> network_;
  std::optional<capnp::RpcSystem<capnp::rpc::twoparty::VatId>> rpc_system_;
};

} // namespace rpcbench
