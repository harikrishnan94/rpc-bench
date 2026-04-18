#pragma once

// Storage abstractions for the benchmarked key-value service. The first
// implementation is intentionally small and in-memory, but the interface keeps
// persistence and alternative engines available as future extensions.

#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>

namespace rpcbench {

struct GetResult {
  // Indicates whether the requested key currently exists.
  bool found = false;

  // Holds the stored value when `found` is true. The value is empty when the
  // key is absent.
  std::string value;
};

class KvStore {
public:
  KvStore() = default;
  KvStore(const KvStore&) = delete;
  KvStore& operator=(const KvStore&) = delete;
  KvStore(KvStore&&) = delete;
  KvStore& operator=(KvStore&&) = delete;
  virtual ~KvStore() = default;

  // Looks up a key and returns the presence flag plus the stored bytes when the
  // key exists.
  [[nodiscard]] virtual GetResult get(std::span<const std::byte> key) = 0;

  // Stores or replaces the value for a key.
  virtual void put(std::span<const std::byte> key, std::span<const std::byte> value) = 0;

  // Removes a key and returns whether a value was removed.
  [[nodiscard]] virtual bool erase(std::span<const std::byte> key) = 0;
};

class InMemoryKvStore final : public KvStore {
  // Simple map-backed store used by the bootstrap server. A mutex keeps the
  // implementation safe if future server code introduces cross-thread access.
public:
  [[nodiscard]] GetResult get(std::span<const std::byte> key) override;

  void put(std::span<const std::byte> key, std::span<const std::byte> value) override;

  [[nodiscard]] bool erase(std::span<const std::byte> key) override;

private:
  std::mutex mutex_;
  std::unordered_map<std::string, std::string> entries_;
};

// Constructs the default in-memory backend used by the server binary.
[[nodiscard]] std::shared_ptr<KvStore> make_in_memory_store();

} // namespace rpcbench
