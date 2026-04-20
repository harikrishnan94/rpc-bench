// Shared RPC session wrappers that hide the stream-vs-message-stream split
// behind one move-only channel type. This keeps the server and benchmark
// frontends focused on lifecycle policy instead of duplicating Cap'n Proto
// bootstrap code.

#include "transport/rpc_session.hpp"

#include <capnp/message.h>
#include <utility>

namespace rpcbench {

RpcChannel::RpcChannel(kj::Own<kj::AsyncIoStream>&& connection) : connection_(kj::mv(connection)) {}

RpcChannel::RpcChannel(kj::Own<capnp::MessageStream>&& message_stream)
    : message_stream_(kj::mv(message_stream)) {}

bool RpcChannel::has_stream() const {
  return connection_.get() != nullptr;
}

kj::AsyncIoStream& RpcChannel::stream() {
  KJ_REQUIRE(connection_.get() != nullptr, "RPC channel is not stream-backed");
  return *connection_;
}

capnp::MessageStream& RpcChannel::message_stream() {
  KJ_REQUIRE(message_stream_.get() != nullptr, "RPC channel is not message-stream-backed");
  return *message_stream_;
}

ClientRpcSession::ClientRpcSession(RpcChannel&& channel) : channel_(kj::mv(channel)) {
  if (channel_.has_stream()) {
    network_.emplace(channel_.stream(), capnp::rpc::twoparty::Side::CLIENT);
  } else {
    network_.emplace(channel_.message_stream(), capnp::rpc::twoparty::Side::CLIENT);
  }

  rpc_system_.emplace(capnp::makeRpcClient(*network_));
  hash_service_.emplace(bootstrap_hash_service(*rpc_system_));
}

HashService::Client& ClientRpcSession::hash_service() {
  return *hash_service_;
}

HashService::Client ClientRpcSession::bootstrap_hash_service(
    capnp::RpcSystem<capnp::rpc::twoparty::VatId>& rpc_system) {
  capnp::MallocMessageBuilder vat_id_builder;
  auto vat_id = vat_id_builder.initRoot<capnp::rpc::twoparty::VatId>();
  vat_id.setSide(capnp::rpc::twoparty::Side::SERVER);
  return rpc_system.bootstrap(vat_id).castAs<HashService>();
}

ServerRpcSession::ServerRpcSession(capnp::Capability::Client bootstrap, RpcChannel&& channel)
    : channel_(kj::mv(channel)) {
  if (channel_.has_stream()) {
    network_.emplace(channel_.stream(), capnp::rpc::twoparty::Side::SERVER);
  } else {
    network_.emplace(channel_.message_stream(), capnp::rpc::twoparty::Side::SERVER);
  }

  rpc_system_.emplace(capnp::makeRpcServer(*network_, kj::mv(bootstrap)));
}

kj::Promise<void> ServerRpcSession::run() {
  return rpc_system_->run().exclusiveJoin(network_->onDisconnect());
}

} // namespace rpcbench
