#pragma once

// Cap'n Proto server bootstrap helpers. This layer adapts the storage interface
// to the generated RPC service and owns the blocking server lifecycle used by
// the standalone server executable and local benchmark spawning.

#include "rpcbench/config.hpp"
#include "rpcbench/storage.hpp"

#include <memory>

namespace rpcbench {

class EzRpcServerRunner {
  // Runs one Cap'n Proto server endpoint backed by one `KvStore`. The current
  // implementation owns one async event loop; higher requested thread counts
  // warn and fall back to this single-threaded server shape.
public:
  EzRpcServerRunner(ServerConfig config, std::shared_ptr<KvStore> store);

  // Returns the bind configuration for this server instance.
  [[nodiscard]] const ServerConfig& config() const;

  // Blocks the calling thread and serves requests until the process exits.
  void run() const;

private:
  ServerConfig config_;
  std::shared_ptr<KvStore> store_;
};

} // namespace rpcbench
