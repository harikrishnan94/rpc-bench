// Shared stream-backed RPC session wrappers. The transport layer hands one
// async stream per connection to these classes, and they own the two-party RPC
// bootstrap for the rest of that connection's lifetime.

#include "transport/rpc_session.hpp"

#include <capnp/message.h>
#include <utility>

namespace rpcbench {

ClientRpcSession::ClientRpcSession(kj::Own<kj::AsyncIoStream>&& connection)
    : connection_(kj::mv(connection)) {
  network_.emplace(*connection_, capnp::rpc::twoparty::Side::CLIENT);
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

ServerRpcSession::ServerRpcSession(capnp::Capability::Client bootstrap,
                                   kj::Own<kj::AsyncIoStream>&& connection)
    : connection_(kj::mv(connection)) {
  network_.emplace(*connection_, capnp::rpc::twoparty::Side::SERVER);
  rpc_system_.emplace(capnp::makeRpcServer(*network_, kj::mv(bootstrap)));
}

kj::Promise<void> ServerRpcSession::run() {
  return rpc_system_->run().exclusiveJoin(network_->onDisconnect());
}

} // namespace rpcbench
