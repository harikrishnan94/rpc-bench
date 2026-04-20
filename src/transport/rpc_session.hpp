#pragma once

// Shared RPC session wrappers for the benchmark and server transport layers.
// These classes own the transport channel plus the Cap'n Proto two-party stack
// together so frontends no longer need to duplicate stream-vs-message-stream
// branching.

#include "protocol/hash_service.capnp.h"

#include <capnp/rpc-twoparty.h>
#include <capnp/serialize-async.h>
#include <kj/async-io.h>
#include <kj/memory.h>
#include <optional>

namespace rpcbench {

class RpcChannel {
  // Holds exactly one transport-backed channel for one RPC session. The
  // channel may be a stream-backed connection or a custom MessageStream-backed
  // local transport.
public:
  explicit RpcChannel(kj::Own<kj::AsyncIoStream>&& connection);
  explicit RpcChannel(kj::Own<capnp::MessageStream>&& message_stream);

  RpcChannel(const RpcChannel&) = delete;
  RpcChannel& operator=(const RpcChannel&) = delete;
  RpcChannel(RpcChannel&& other) noexcept = default;
  RpcChannel& operator=(RpcChannel&& other) noexcept = default;

  [[nodiscard]] bool has_stream() const;
  [[nodiscard]] kj::AsyncIoStream& stream();
  [[nodiscard]] capnp::MessageStream& message_stream();

private:
  kj::Own<kj::AsyncIoStream> connection_;
  kj::Own<capnp::MessageStream> message_stream_;
};

class ClientRpcSession {
  // Owns one client-side two-party RPC stack for the lifetime of one benchmark
  // worker connection.
public:
  explicit ClientRpcSession(RpcChannel&& channel);

  [[nodiscard]] HashService::Client& hash_service();

private:
  [[nodiscard]] static HashService::Client
  bootstrap_hash_service(capnp::RpcSystem<capnp::rpc::twoparty::VatId>& rpc_system);

  RpcChannel channel_;
  std::optional<capnp::TwoPartyVatNetwork> network_;
  std::optional<capnp::RpcSystem<capnp::rpc::twoparty::VatId>> rpc_system_;
  std::optional<HashService::Client> hash_service_;
};

class ServerRpcSession {
  // Owns one server-side two-party RPC stack for the lifetime of one accepted
  // benchmark session or one local MessageStream slot.
public:
  ServerRpcSession(capnp::Capability::Client bootstrap, RpcChannel&& channel);

  [[nodiscard]] kj::Promise<void> run();

private:
  RpcChannel channel_;
  std::optional<capnp::TwoPartyVatNetwork> network_;
  std::optional<capnp::RpcSystem<capnp::rpc::twoparty::VatId>> rpc_system_;
};

} // namespace rpcbench
