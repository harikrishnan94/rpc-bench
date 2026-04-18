// In-memory key-value storage used by the bootstrap server. The implementation
// is intentionally direct so the benchmark measures RPC behavior without adding
// storage-engine complexity that belongs in a later milestone.

#include "rpcbench/storage.hpp"

namespace rpcbench {
namespace {

std::string bytes_to_string(std::span<const std::byte> bytes) {
  const auto* data = reinterpret_cast<const char*>(bytes.data());
  return std::string(data, bytes.size());
}

} // namespace

GetResult InMemoryKvStore::get(std::span<const std::byte> key) {
  const auto owned_key = bytes_to_string(key);
  const std::lock_guard guard(mutex_);

  const auto it = entries_.find(owned_key);
  if (it == entries_.end()) {
    return GetResult{
        .found = false,
        .value = {},
    };
  }

  return GetResult{
      .found = true,
      .value = it->second,
  };
}

void InMemoryKvStore::put(std::span<const std::byte> key, std::span<const std::byte> value) {
  const std::lock_guard guard(mutex_);
  entries_[bytes_to_string(key)] = bytes_to_string(value);
}

bool InMemoryKvStore::erase(std::span<const std::byte> key) {
  const std::lock_guard guard(mutex_);
  return entries_.erase(bytes_to_string(key)) > 0;
}

std::shared_ptr<KvStore> make_in_memory_store() {
  return std::make_shared<InMemoryKvStore>();
}

} // namespace rpcbench
