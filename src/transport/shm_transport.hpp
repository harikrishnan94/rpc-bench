#pragma once

// Shared-memory transport helpers for the hybrid Cap'n Proto MessageStream
// path. This header keeps the ring layout, wake primitives, and side-band
// control framing inside the transport layer so frontends can reuse the same
// local transport semantics in both connect and spawn-local modes.

#include <atomic>
#include <capnp/serialize-async.h>
#include <capnp/serialize.h>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <kj/async-io.h>
#include <kj/async.h>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rpcbench {

namespace detail {
struct ShmRingHeader;
struct ShmTransportHeader;
struct ShmSlotDescriptor;
} // namespace detail

struct ShmTransportOptions {
  // Describes the fixed shared-memory layout for all request/response slots.
  // The layout is deliberately explicit so both processes can derive the same
  // offsets without consulting any frontend-specific policy.
  std::size_t slot_count = 0;
  std::size_t request_ring_capacity = 0;
  std::size_t response_ring_capacity = 0;
  std::size_t request_max_message_bytes = 0;
  std::size_t response_max_message_bytes = 0;

  // Validates the numeric limits required by the mapped layout and control
  // socket framing.
  [[nodiscard]] std::expected<void, std::string> validate() const;
};

// Returns the standard shared-memory layout used by rpc-bench's hybrid local
// transport for one fixed worker-slot count.
[[nodiscard]] ShmTransportOptions default_shm_transport_options(std::size_t slot_count);

class ShmMessageRingView {
  // Non-owning view over one single-producer/single-consumer ring inside the
  // shared-memory region. Each push copies one serialized message into one
  // slot and each pop copies it back out, which keeps ownership clear for the
  // MessageStream reader wrapper and avoids exposing mapped memory lifetimes to
  // the frontends.
public:
  ShmMessageRingView() = default;

  [[nodiscard]] bool valid() const;
  [[nodiscard]] std::size_t capacity() const;
  [[nodiscard]] std::size_t max_message_bytes() const;
  [[nodiscard]] std::size_t buffer_bytes() const;
  [[nodiscard]] std::size_t queued_messages() const;
  [[nodiscard]] std::size_t available_slots() const;
  [[nodiscard]] bool empty() const;
  [[nodiscard]] bool writer_closed() const;
  [[nodiscard]] bool drained_and_closed() const;

  // Copies one serialized message into the next free slot. Returns an error
  // when the ring is full, closed, or the message is larger than the slot
  // payload budget.
  [[nodiscard]] std::expected<void, std::string>
  push_bytes(std::span<const std::byte> message) const;

  // Copies and removes the next serialized message if one is available.
  // `std::nullopt` means the ring is currently empty.
  [[nodiscard]] std::expected<std::optional<std::vector<std::byte>>, std::string>
  try_pop_bytes() const;

  // Marks the producer end closed so the reader can convert an empty ring into
  // EOF once all queued messages have been drained.
  void close_writer() const;

private:
  friend class ShmTransportRegion;

  ShmMessageRingView(detail::ShmRingHeader* header, std::byte* slots) noexcept;

  detail::ShmRingHeader* header_ = nullptr;
  std::byte* slots_ = nullptr;
};

struct ShmSlotRings {
  // Raw request/response views for one logical slot. The benchmark writes to
  // `request` and reads from `response`; the server does the opposite.
  ShmMessageRingView request;
  ShmMessageRingView response;
};

struct ShmEndpointRings {
  // Directional ring views for one process-local endpoint. This keeps the
  // MessageStream constructor from needing to know whether it is running on the
  // benchmark side or the server side.
  ShmMessageRingView inbound;
  ShmMessageRingView outbound;
};

class ShmTransportRegion {
  // Owns one mapped POSIX shared-memory region and exposes request/response
  // ring views per logical slot. The mapping knows only about transport layout
  // and lifetime, which lets the main agent integrate URI, CLI, and process
  // management changes elsewhere without rewriting these helpers.
public:
  ShmTransportRegion() = default;
  ~ShmTransportRegion();

  ShmTransportRegion(ShmTransportRegion&& other) noexcept;
  ShmTransportRegion& operator=(ShmTransportRegion&& other) noexcept;

  ShmTransportRegion(const ShmTransportRegion&) = delete;
  ShmTransportRegion& operator=(const ShmTransportRegion&) = delete;

  // Creates a brand-new shared-memory object, sizes it, maps it, and
  // initializes the slot layout.
  [[nodiscard]] static std::expected<ShmTransportRegion, std::string>
  create(std::string_view logical_name, const ShmTransportOptions& options);

  // Opens and maps an existing shared-memory object, validating that its
  // recorded layout matches the expected on-disk format.
  [[nodiscard]] static std::expected<ShmTransportRegion, std::string>
  open(std::string_view logical_name);

  [[nodiscard]] const ShmTransportOptions& options() const;
  [[nodiscard]] std::string_view shared_memory_name() const;
  [[nodiscard]] std::size_t mapped_bytes() const;

  [[nodiscard]] ShmSlotRings slot(std::size_t slot_index) const;
  [[nodiscard]] ShmEndpointRings benchmark_endpoint(std::size_t slot_index) const;
  [[nodiscard]] ShmEndpointRings server_endpoint(std::size_t slot_index) const;

  // Removes the named POSIX shared-memory object. Existing mappings stay valid
  // until all descriptors are closed, matching normal shm semantics.
  [[nodiscard]] std::expected<void, std::string> unlink() const;

private:
  ShmTransportRegion(int fd,
                     void* mapping,
                     std::size_t mapped_bytes,
                     std::string shm_name,
                     ShmTransportOptions options,
                     detail::ShmTransportHeader* header,
                     detail::ShmSlotDescriptor* descriptors) noexcept;

  void reset() noexcept;
  [[nodiscard]] ShmMessageRingView ring_from_offset(std::uint64_t offset) const;

  int fd_ = -1;
  void* mapping_ = nullptr;
  std::size_t mapped_bytes_ = 0;
  std::string shm_name_;
  ShmTransportOptions options_;
  detail::ShmTransportHeader* header_ = nullptr;
  detail::ShmSlotDescriptor* descriptors_ = nullptr;
};

enum class ControlMessageKind : std::uint8_t {
  init = 1,
  wake = 2,
  attach = 3,
};

struct ControlMessage {
  // Fixed-size control frame carried over the hybrid wakeup socket. Direction
  // determines whether the slot refers to the request or response ring, so the
  // frame only needs a small message kind plus one 16-bit value. For `attach`,
  // `slot_index` carries the active slot count for the benchmark run.
  ControlMessageKind kind = ControlMessageKind::wake;
  std::uint8_t reserved = 0;
  std::uint16_t slot_index = 0;
};

static_assert(sizeof(ControlMessage) == 4);

class WakeListener {
  // Small abstraction used by ShmMessageStream so reads can sleep on KJ
  // promises instead of polling shared memory. Concrete implementations choose
  // whether wakeups stay on one KJ loop thread or cross from a helper thread
  // into a worker thread.
public:
  WakeListener() = default;
  virtual ~WakeListener() noexcept(false) = default;

  WakeListener(const WakeListener&) = delete;
  WakeListener& operator=(const WakeListener&) = delete;
  WakeListener(WakeListener&&) = delete;
  WakeListener& operator=(WakeListener&&) = delete;

  [[nodiscard]] virtual kj::Promise<void> wait() = 0;
  virtual void close(std::string_view reason) = 0;
};

class SameThreadWakeListener final : public WakeListener {
  // Delivers wakeups entirely on one KJ event-loop thread. The server-side
  // dispatcher and the waiting stream both live on that loop, so the class can
  // stay lightweight while still coalescing missed or early signals.
public:
  [[nodiscard]] kj::Promise<void> wait() override;
  void close(std::string_view reason) override;

  // Records one wakeup or resolves the current waiter immediately.
  void signal();

private:
  std::size_t pending_signals_ = 0;
  bool closed_ = false;
  std::string close_reason_;
  kj::Own<kj::PromiseFulfiller<void>> waiter_;
};

class CrossThreadWakeListener final : public WakeListener {
  // Accepts signals from a non-KJ helper thread and resolves waiters on the
  // owning worker thread with `newPromiseAndCrossThreadFulfiller()`. This lets
  // benchmark workers keep a promise-based read path while a dedicated broker
  // thread owns the blocking control-socket read loop.
public:
  [[nodiscard]] kj::Promise<void> wait() override;
  void close(std::string_view reason) override;

  // Records one wakeup or resolves the current waiter from another thread.
  void signal();

private:
  mutable std::mutex mutex_;
  std::size_t pending_signals_ = 0;
  bool closed_ = false;
  std::string close_reason_;
  kj::Own<kj::CrossThreadPromiseFulfiller<void>> waiter_;
};

class ControlMessageSender {
  // Narrow write-side interface for fixed-size init/wake control frames.
  // MessageStream only depends on this surface, which keeps its notification
  // logic independent from whether the sender is a raw socket wrapper or a
  // larger broker object.
public:
  ControlMessageSender() = default;
  virtual ~ControlMessageSender() = default;

  ControlMessageSender(const ControlMessageSender&) = delete;
  ControlMessageSender& operator=(const ControlMessageSender&) = delete;
  ControlMessageSender(ControlMessageSender&&) = delete;
  ControlMessageSender& operator=(ControlMessageSender&&) = delete;

  [[nodiscard]] virtual std::expected<void, std::string> send(ControlMessageKind kind,
                                                              std::size_t slot_index) = 0;

  [[nodiscard]] std::expected<void, std::string> send_attach(std::size_t slot_count);
  [[nodiscard]] std::expected<void, std::string> send_init(std::size_t slot_index);
  [[nodiscard]] std::expected<void, std::string> send_wake(std::size_t slot_index);
};

class HybridControlSocket final : public ControlMessageSender {
  // Owns one wakeup socket endpoint and provides serialized writes for small
  // init/wake frames. The serialization lives here so multiple components can
  // share the same control channel without interleaving partial frames.
public:
  explicit HybridControlSocket(int fd) noexcept;
  ~HybridControlSocket() override;

  HybridControlSocket(const HybridControlSocket&) = delete;
  HybridControlSocket& operator=(const HybridControlSocket&) = delete;
  HybridControlSocket(HybridControlSocket&&) = delete;
  HybridControlSocket& operator=(HybridControlSocket&&) = delete;

  [[nodiscard]] int fd() const noexcept;
  [[nodiscard]] bool valid() const noexcept;

  [[nodiscard]] std::expected<void, std::string> send(ControlMessageKind kind,
                                                      std::size_t slot_index) override;

  // Shuts down both directions so any blocking reader can notice teardown.
  [[nodiscard]] std::expected<void, std::string> shutdown() const;

private:
  int fd_ = -1;
  mutable std::mutex send_mutex_;
};

class ParentWakeBroker final : public ControlMessageSender {
  // Owns the benchmark-side control channel. A dedicated std::thread performs
  // the blocking reads and fan-outs per-slot wakeups into cross-thread KJ
  // listeners, while the object also exposes the serialized send path that the
  // benchmark side uses for init and wake notifications.
public:
  ParentWakeBroker(int fd, std::span<CrossThreadWakeListener*> listeners);
  ~ParentWakeBroker() noexcept override;

  ParentWakeBroker(const ParentWakeBroker&) = delete;
  ParentWakeBroker& operator=(const ParentWakeBroker&) = delete;
  ParentWakeBroker(ParentWakeBroker&&) = delete;
  ParentWakeBroker& operator=(ParentWakeBroker&&) = delete;

  // Starts the background broker thread once. Fails if thread creation fails.
  [[nodiscard]] std::expected<void, std::string> start();
  void stop() noexcept;

  [[nodiscard]] std::expected<void, std::string> send(ControlMessageKind kind,
                                                      std::size_t slot_index) override;

  // Returns the last broker-thread error, if the read loop terminated
  // unexpectedly.
  [[nodiscard]] std::optional<std::string> background_error() const;

private:
  void run();
  void record_background_error(std::string error);
  void close_listeners(std::string_view reason);

  HybridControlSocket socket_;
  std::vector<CrossThreadWakeListener*> listeners_;
  std::thread thread_;
  bool started_ = false;
  std::atomic<bool> stopping_ = false;
  mutable std::mutex error_mutex_;
  std::optional<std::string> background_error_;
};

class ServerControlDispatcher {
  // Owns the server-side read loop for the hybrid wakeup socket. It stays on
  // the KJ thread, signals same-thread wake listeners for each slot, and also
  // tracks the attach + init handshake so server integration can wait until
  // the connected benchmark has announced the active slot count before exposing
  // those slots to RPC code.
public:
  ServerControlDispatcher(kj::AsyncIoStream& stream, std::span<SameThreadWakeListener*> listeners);

  // Runs the control-socket read loop until EOF or an error occurs.
  [[nodiscard]] kj::Promise<void> run();

  // Resolves once the benchmark has announced its active slot count and one
  // init frame has been observed for every active slot.
  [[nodiscard]] kj::Promise<void> initialized();

  // Returns the active slot count announced by the connected benchmark.
  [[nodiscard]] std::size_t active_slot_count() const;

private:
  [[nodiscard]] kj::Promise<void> pump_one();
  [[nodiscard]] std::expected<void, std::string> handle_message(const ControlMessage& message);
  void close_listeners(std::string_view reason);
  void reject_init(std::string_view reason);

  kj::AsyncIoStream& stream_;
  std::vector<SameThreadWakeListener*> listeners_;
  std::vector<std::uint8_t> init_seen_;
  std::size_t init_count_ = 0;
  std::size_t active_slot_count_ = 0;
  bool started_ = false;
  bool attach_seen_ = false;
  ControlMessage read_buffer_{};
  kj::Own<kj::PromiseFulfiller<void>> init_fulfiller_;
  kj::ForkedPromise<void> init_ready_ = nullptr;
};

class ShmMessageStream final : public capnp::MessageStream {
  // Adapts one directional shared-memory slot pair into Cap'n Proto's
  // MessageStream interface. Reads copy serialized messages out of the inbound
  // ring into owned flat arrays before constructing readers, while writes
  // flatten outbound messages into one ring slot each and notify the peer over
  // the control socket instead of polling.
public:
  ShmMessageStream(std::size_t slot_index,
                   ShmEndpointRings rings,
                   WakeListener& inbound_wake,
                   ControlMessageSender& outbound_wake_sender);

  kj::Promise<kj::Maybe<capnp::MessageReaderAndFds>>
  tryReadMessage(kj::ArrayPtr<kj::AutoCloseFd> fd_space,
                 capnp::ReaderOptions options = capnp::ReaderOptions(),
                 kj::ArrayPtr<capnp::word> scratch_space = nullptr) override;

  kj::Promise<void>
  writeMessage(kj::ArrayPtr<const int> fds,
               kj::ArrayPtr<const kj::ArrayPtr<const capnp::word>> segments) override;

  kj::Promise<void> writeMessages(
      kj::ArrayPtr<kj::ArrayPtr<const kj::ArrayPtr<const capnp::word>>> messages) override;

  kj::Maybe<int> getSendBufferSize() override;
  kj::Promise<void> end() override;

  using MessageStream::tryReadMessage;
  using MessageStream::writeMessage;

private:
  [[nodiscard]] kj::Promise<kj::Maybe<capnp::MessageReaderAndFds>>
  wait_for_message(kj::ArrayPtr<kj::AutoCloseFd> fd_space,
                   capnp::ReaderOptions options,
                   kj::ArrayPtr<capnp::word> scratch_space);

  [[nodiscard]] std::expected<void, std::string> notify_peer(ControlMessageKind kind) const;

  std::size_t slot_index_ = 0;
  ShmMessageRingView inbound_ring_;
  ShmMessageRingView outbound_ring_;
  WakeListener* inbound_wake_ = nullptr;
  ControlMessageSender* outbound_wake_sender_ = nullptr;
  bool read_wait_active_ = false;
  bool ended_ = false;
};

} // namespace rpcbench
