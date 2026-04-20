// Shared-memory transport helpers for the hybrid Cap'n Proto MessageStream
// path. The implementation keeps the mapped ring layout, wake primitives, and
// control-socket framing self-contained so connect-mode and spawn-local shared
// memory reuse one transport protocol.

#include "transport/shm_transport.hpp"

#include "protocol/service.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <kj/debug.h>
#include <kj/string.h>
#include <limits>
#include <memory>
#include <new>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace rpcbench {

namespace detail {

struct alignas(64) ShmTransportHeader {
  std::array<char, 8> magic{};
  std::uint32_t version = 0;
  std::uint32_t slot_count = 0;
  std::uint32_t request_ring_capacity = 0;
  std::uint32_t response_ring_capacity = 0;
  std::uint32_t request_max_message_bytes = 0;
  std::uint32_t response_max_message_bytes = 0;
  std::uint64_t total_mapped_bytes = 0;
};

struct ShmSlotDescriptor {
  std::uint64_t request_ring_offset = 0;
  std::uint64_t response_ring_offset = 0;
};

struct alignas(64) ShmRingHeader {
  std::atomic<std::uint64_t> head = 0;
  std::atomic<std::uint64_t> tail = 0;
  std::atomic<std::uint32_t> writer_closed = 0;
  std::uint32_t capacity = 0;
  std::uint32_t max_message_bytes = 0;
  std::uint32_t slot_stride_bytes = 0;
  std::uint32_t reserved = 0;
};

} // namespace detail

namespace {

inline constexpr std::array<char, 8> kShmMagic = {'r', 'p', 'c', 'b', 's', 'h', 'm', '1'};
inline constexpr std::uint32_t kShmVersion = 1;
inline constexpr std::size_t kRingHeaderAlignment = 64;
inline constexpr std::size_t kDefaultRingCapacity = 8;
inline constexpr std::size_t kDefaultMessageSlackBytes = 64U * 1024U;

struct RingLayout {
  std::size_t slot_stride_bytes = 0;
  std::size_t total_bytes = 0;
};

struct RingShape {
  std::size_t capacity = 0;
  std::size_t max_message_bytes = 0;
};

struct RingInitialization {
  std::size_t capacity = 0;
  std::size_t max_message_bytes = 0;
  std::size_t slot_stride_bytes = 0;
};

struct TransportLayout {
  std::size_t descriptors_offset = 0;
  std::size_t total_bytes = 0;
  std::size_t request_slot_stride_bytes = 0;
  std::size_t response_slot_stride_bytes = 0;
  std::vector<detail::ShmSlotDescriptor> descriptors;
};

class OwnedFlatArrayMessageReader final : public capnp::MessageReader {
  // Owns the copied flat array backing one inbound shared-memory message. This
  // wrapper lets the MessageStream hand Cap'n Proto a stable MessageReader
  // without keeping the consumer pinned to one ring slot's mapped memory.
public:
  OwnedFlatArrayMessageReader(kj::Array<capnp::word>&& words, capnp::ReaderOptions options)
      : capnp::MessageReader(options), words_(kj::mv(words)), reader_(words_.asPtr(), options) {}

  kj::ArrayPtr<const capnp::word> getSegment(uint id) override {
    return reader_.getSegment(id);
  }

private:
  kj::Array<capnp::word> words_;
  capnp::FlatArrayMessageReader reader_;
};

[[nodiscard]] std::string system_error_message(std::string_view action, int error_number) {
  const std::error_code error(error_number, std::generic_category());
  return std::string(action) + ": " + error.message();
}

[[nodiscard]] std::expected<std::string, std::string>
normalize_shm_name(std::string_view logical_name) {
  if (logical_name.empty()) {
    return std::unexpected("shared-memory logical name must not be empty");
  }

  std::string normalized(logical_name);
  if (!normalized.starts_with('/')) {
    normalized.insert(normalized.begin(), '/');
  }

  if (normalized.size() == 1) {
    return std::unexpected("shared-memory logical name must not be '/'");
  }

  if (normalized.find('/', 1) != std::string::npos) {
    return std::unexpected(
        std::format("shared-memory logical name must not contain '/': {}", logical_name));
  }

  return normalized;
}

[[nodiscard]] std::expected<std::uint32_t, std::string> narrow_u32(std::size_t value,
                                                                   std::string_view name) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(std::format("{} exceeds uint32_t limits", name));
  }
  return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::expected<std::uint16_t, std::string> narrow_u16(std::size_t value,
                                                                   std::string_view name) {
  if (value > std::numeric_limits<std::uint16_t>::max()) {
    return std::unexpected(std::format("{} exceeds uint16_t limits", name));
  }
  return static_cast<std::uint16_t>(value);
}

[[nodiscard]] std::expected<std::size_t, std::string>
checked_add(std::size_t lhs, std::size_t rhs, std::string_view what) {
  if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
    return std::unexpected(std::format("{} overflows", what));
  }
  return lhs + rhs;
}

[[nodiscard]] std::expected<std::size_t, std::string>
checked_mul(std::size_t lhs, std::size_t rhs, std::string_view what) {
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    return std::unexpected(std::format("{} overflows", what));
  }
  return lhs * rhs;
}

[[nodiscard]] std::expected<std::size_t, std::string>
align_up(std::size_t value, std::size_t alignment, std::string_view what) {
  KJ_REQUIRE(alignment != 0, "alignment must be non-zero");

  const auto remainder = value % alignment;
  if (remainder == 0) {
    return value;
  }
  return checked_add(value, alignment - remainder, what);
}

[[nodiscard]] std::expected<RingLayout, std::string>
compute_ring_layout(RingShape ring, std::string_view ring_name) {
  auto slot_bytes = checked_add(
      sizeof(std::uint32_t) * 2, ring.max_message_bytes, std::format("{} slot size", ring_name));
  if (!slot_bytes) {
    return std::unexpected(slot_bytes.error());
  }

  auto slot_stride =
      align_up(*slot_bytes, alignof(capnp::word), std::format("{} slot alignment", ring_name));
  if (!slot_stride) {
    return std::unexpected(slot_stride.error());
  }

  auto slots_total =
      checked_mul(ring.capacity, *slot_stride, std::format("{} ring payload", ring_name));
  if (!slots_total) {
    return std::unexpected(slots_total.error());
  }

  auto total = checked_add(
      sizeof(detail::ShmRingHeader), *slots_total, std::format("{} ring total size", ring_name));
  if (!total) {
    return std::unexpected(total.error());
  }

  return RingLayout{
      .slot_stride_bytes = *slot_stride,
      .total_bytes = *total,
  };
}

[[nodiscard]] std::expected<TransportLayout, std::string>
compute_transport_layout(const ShmTransportOptions& options) {
  if (auto valid = options.validate(); !valid) {
    return std::unexpected(valid.error());
  }

  auto request_layout = compute_ring_layout(
      RingShape{
          .capacity = options.request_ring_capacity,
          .max_message_bytes = options.request_max_message_bytes,
      },
      "request");
  if (!request_layout) {
    return std::unexpected(request_layout.error());
  }

  auto response_layout = compute_ring_layout(
      RingShape{
          .capacity = options.response_ring_capacity,
          .max_message_bytes = options.response_max_message_bytes,
      },
      "response");
  if (!response_layout) {
    return std::unexpected(response_layout.error());
  }

  auto descriptors_offset = align_up(sizeof(detail::ShmTransportHeader),
                                     alignof(detail::ShmSlotDescriptor),
                                     "slot descriptor table offset");
  if (!descriptors_offset) {
    return std::unexpected(descriptors_offset.error());
  }

  auto descriptor_bytes = checked_mul(
      options.slot_count, sizeof(detail::ShmSlotDescriptor), "slot descriptor table size");
  if (!descriptor_bytes) {
    return std::unexpected(descriptor_bytes.error());
  }

  auto current = checked_add(*descriptors_offset, *descriptor_bytes, "ring area offset");
  if (!current) {
    return std::unexpected(current.error());
  }

  auto aligned_current = align_up(*current, kRingHeaderAlignment, "ring area alignment");
  if (!aligned_current) {
    return std::unexpected(aligned_current.error());
  }

  std::vector<detail::ShmSlotDescriptor> descriptors;
  descriptors.reserve(options.slot_count);

  std::size_t offset = *aligned_current;
  for (std::size_t slot_index = 0; slot_index < options.slot_count; ++slot_index) {
    const auto request_offset = offset;
    auto after_request = checked_add(offset, request_layout->total_bytes, "request ring end");
    if (!after_request) {
      return std::unexpected(after_request.error());
    }

    auto request_aligned =
        align_up(*after_request, kRingHeaderAlignment, "response ring alignment");
    if (!request_aligned) {
      return std::unexpected(request_aligned.error());
    }

    const auto response_offset = *request_aligned;
    auto after_response =
        checked_add(response_offset, response_layout->total_bytes, "response ring end");
    if (!after_response) {
      return std::unexpected(after_response.error());
    }

    auto response_aligned = align_up(*after_response, kRingHeaderAlignment, "next ring alignment");
    if (!response_aligned) {
      return std::unexpected(response_aligned.error());
    }

    descriptors.push_back(detail::ShmSlotDescriptor{
        .request_ring_offset = request_offset,
        .response_ring_offset = response_offset,
    });

    offset = *response_aligned;
  }

  return TransportLayout{
      .descriptors_offset = *descriptors_offset,
      .total_bytes = offset,
      .request_slot_stride_bytes = request_layout->slot_stride_bytes,
      .response_slot_stride_bytes = response_layout->slot_stride_bytes,
      .descriptors = kj::mv(descriptors),
  };
}

void close_fd(int& fd) noexcept {
  if (fd >= 0) {
    static_cast<void>(::close(fd));
    fd = -1;
  }
}

void close_mapping(void*& mapping, std::size_t& mapped_bytes) noexcept {
  if (mapping != nullptr) {
    static_cast<void>(::munmap(mapping, mapped_bytes));
    mapping = nullptr;
    mapped_bytes = 0;
  }
}

void initialize_ring(std::byte* base, RingInitialization ring) {
  auto* header = new (base) detail::ShmRingHeader;
  header->head.store(0, std::memory_order_relaxed);
  header->tail.store(0, std::memory_order_relaxed);
  header->writer_closed.store(0, std::memory_order_relaxed);
  header->capacity = static_cast<std::uint32_t>(ring.capacity);
  header->max_message_bytes = static_cast<std::uint32_t>(ring.max_message_bytes);
  header->slot_stride_bytes = static_cast<std::uint32_t>(ring.slot_stride_bytes);
  header->reserved = 0;
}

[[nodiscard]] std::expected<ControlMessage, std::string>
make_control_message(ControlMessageKind kind, std::size_t slot_index) {
  auto narrowed = narrow_u16(slot_index, "control-message slot index");
  if (!narrowed) {
    return std::unexpected(narrowed.error());
  }

  return ControlMessage{
      .kind = kind,
      .reserved = 0,
      .slot_index = *narrowed,
  };
}

[[nodiscard]] std::expected<void, std::string> read_exact(int fd, std::span<std::byte> buffer) {
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const auto rc = ::read(fd, buffer.data() + offset, buffer.size() - offset);
    if (rc == 0) {
      return std::unexpected("control socket closed");
    }
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(system_error_message("control socket read failed", errno));
    }
    offset += static_cast<std::size_t>(rc);
  }
  return {};
}

[[nodiscard]] std::expected<void, std::string> send_all(int fd, std::span<const std::byte> buffer) {
  std::size_t offset = 0;
  while (offset < buffer.size()) {
#ifdef MSG_NOSIGNAL
    constexpr int send_flags = MSG_NOSIGNAL;
#else
    constexpr int send_flags = 0;
#endif
    const auto rc = ::send(fd, buffer.data() + offset, buffer.size() - offset, send_flags);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(system_error_message("control socket write failed", errno));
    }
    offset += static_cast<std::size_t>(rc);
  }
  return {};
}

template <typename Listener>
[[nodiscard]] std::expected<void, std::string>
signal_control_message(std::span<Listener*> listeners, const ControlMessage& message) {
  const auto slot_index = static_cast<std::size_t>(message.slot_index);
  if (slot_index >= listeners.size()) {
    return std::unexpected(
        std::format("control message referenced out-of-range slot {}", slot_index));
  }

  auto* listener = listeners[slot_index];
  if (listener == nullptr) {
    return std::unexpected(std::format("control message referenced unbound slot {}", slot_index));
  }

  switch (message.kind) {
  case ControlMessageKind::init:
  case ControlMessageKind::wake:
    listener->signal();
    return {};
  case ControlMessageKind::attach:
    return std::unexpected("attach control messages are only valid on the server side");
  }

  return std::unexpected(
      std::format("unknown control message kind {}", std::to_underlying(message.kind)));
}

[[nodiscard]] std::expected<capnp::MessageReaderAndFds, std::string>
decode_owned_message(const std::vector<std::byte>& message, capnp::ReaderOptions options) {
  if (message.empty()) {
    return std::unexpected("shared-memory message must not be empty");
  }

  if ((message.size() % sizeof(capnp::word)) != 0) {
    return std::unexpected(
        std::format("shared-memory message size must be word-aligned: {}", message.size()));
  }

  const auto word_count = message.size() / sizeof(capnp::word);
  auto words = kj::heapArray<capnp::word>(word_count);
  std::memcpy(words.begin(), message.data(), message.size());

  return capnp::MessageReaderAndFds{
      .reader = kj::heap<OwnedFlatArrayMessageReader>(kj::mv(words), options),
      .fds = nullptr,
  };
}

} // namespace

std::expected<void, std::string> ShmTransportOptions::validate() const {
  if (slot_count == 0) {
    return std::unexpected("shared-memory transport requires at least one slot");
  }
  if (request_ring_capacity == 0) {
    return std::unexpected("request ring capacity must be greater than zero");
  }
  if (response_ring_capacity == 0) {
    return std::unexpected("response ring capacity must be greater than zero");
  }
  if (request_max_message_bytes == 0) {
    return std::unexpected("request max message size must be greater than zero");
  }
  if (response_max_message_bytes == 0) {
    return std::unexpected("response max message size must be greater than zero");
  }
  if (slot_count > std::numeric_limits<std::uint16_t>::max()) {
    return std::unexpected("shared-memory slot count exceeds control-message limits");
  }
  if (!narrow_u32(request_ring_capacity, "request ring capacity")) {
    return std::unexpected("request ring capacity exceeds uint32_t limits");
  }
  if (!narrow_u32(response_ring_capacity, "response ring capacity")) {
    return std::unexpected("response ring capacity exceeds uint32_t limits");
  }
  if (!narrow_u32(request_max_message_bytes, "request max message size")) {
    return std::unexpected("request max message size exceeds uint32_t limits");
  }
  if (!narrow_u32(response_max_message_bytes, "response max message size")) {
    return std::unexpected("response max message size exceeds uint32_t limits");
  }
  return {};
}

ShmTransportOptions default_shm_transport_options(std::size_t slot_count) {
  return ShmTransportOptions{
      .slot_count = slot_count,
      .request_ring_capacity = kDefaultRingCapacity,
      .response_ring_capacity = kDefaultRingCapacity,
      .request_max_message_bytes =
          static_cast<std::size_t>(kMaxPayloadSizeBytes) + kDefaultMessageSlackBytes,
      .response_max_message_bytes =
          static_cast<std::size_t>(kMaxPayloadSizeBytes) + kDefaultMessageSlackBytes,
  };
}

ShmMessageRingView::ShmMessageRingView(detail::ShmRingHeader* header, std::byte* slots) noexcept
    : header_(header), slots_(slots) {}

bool ShmMessageRingView::valid() const {
  return header_ != nullptr && slots_ != nullptr;
}

std::size_t ShmMessageRingView::capacity() const {
  return valid() ? header_->capacity : 0;
}

std::size_t ShmMessageRingView::max_message_bytes() const {
  return valid() ? header_->max_message_bytes : 0;
}

std::size_t ShmMessageRingView::buffer_bytes() const {
  return capacity() * max_message_bytes();
}

std::size_t ShmMessageRingView::queued_messages() const {
  if (!valid()) {
    return 0;
  }

  const auto head = header_->head.load(std::memory_order_acquire);
  const auto tail = header_->tail.load(std::memory_order_acquire);
  if (tail < head) {
    return 0;
  }

  return static_cast<std::size_t>(tail - head);
}

std::size_t ShmMessageRingView::available_slots() const {
  const auto queued = queued_messages();
  const auto ring_capacity = capacity();
  if (queued >= ring_capacity) {
    return 0;
  }
  return ring_capacity - queued;
}

bool ShmMessageRingView::empty() const {
  return queued_messages() == 0;
}

bool ShmMessageRingView::writer_closed() const {
  return valid() && header_->writer_closed.load(std::memory_order_acquire) != 0;
}

bool ShmMessageRingView::drained_and_closed() const {
  return writer_closed() && empty();
}

std::expected<void, std::string>
ShmMessageRingView::push_bytes(std::span<const std::byte> message) const {
  if (!valid()) {
    return std::unexpected("shared-memory ring view is not initialized");
  }
  if (header_->writer_closed.load(std::memory_order_acquire) != 0) {
    return std::unexpected("shared-memory ring writer is closed");
  }
  if (message.size() > header_->max_message_bytes) {
    return std::unexpected(std::format("shared-memory message exceeds slot size: {} > {}",
                                       message.size(),
                                       header_->max_message_bytes));
  }

  const auto head = header_->head.load(std::memory_order_acquire);
  const auto tail = header_->tail.load(std::memory_order_relaxed);
  if (tail < head || (tail - head) > header_->capacity) {
    return std::unexpected("shared-memory ring counters are corrupted");
  }
  if ((tail - head) >= header_->capacity) {
    return std::unexpected("shared-memory ring is full");
  }

  const auto slot_index = static_cast<std::size_t>(tail % header_->capacity);
  auto* slot = slots_ + (slot_index * header_->slot_stride_bytes);
  auto* size_ptr = reinterpret_cast<std::uint32_t*>(slot);
  auto* payload = slot + (sizeof(std::uint32_t) * 2);

  *size_ptr = static_cast<std::uint32_t>(message.size());
  std::memcpy(payload, message.data(), message.size());
  header_->tail.store(tail + 1, std::memory_order_release);
  return {};
}

std::expected<std::optional<std::vector<std::byte>>, std::string>
ShmMessageRingView::try_pop_bytes() const {
  if (!valid()) {
    return std::unexpected("shared-memory ring view is not initialized");
  }

  const auto tail = header_->tail.load(std::memory_order_acquire);
  const auto head = header_->head.load(std::memory_order_relaxed);
  if (tail < head || (tail - head) > header_->capacity) {
    return std::unexpected("shared-memory ring counters are corrupted");
  }
  if (head == tail) {
    return std::optional<std::vector<std::byte>>();
  }

  const auto slot_index = static_cast<std::size_t>(head % header_->capacity);
  auto* slot = slots_ + (slot_index * header_->slot_stride_bytes);
  auto* size_ptr = reinterpret_cast<const std::uint32_t*>(slot);
  const auto size_bytes = static_cast<std::size_t>(*size_ptr);
  if (size_bytes > header_->max_message_bytes) {
    return std::unexpected(std::format("shared-memory ring slot exceeds maximum size: {} > {}",
                                       size_bytes,
                                       header_->max_message_bytes));
  }

  const auto* payload = slot + (sizeof(std::uint32_t) * 2);
  std::vector<std::byte> message(size_bytes);
  std::memcpy(message.data(), payload, size_bytes);
  header_->head.store(head + 1, std::memory_order_release);
  return std::optional<std::vector<std::byte>>(kj::mv(message));
}

void ShmMessageRingView::close_writer() const {
  if (valid()) {
    header_->writer_closed.store(1, std::memory_order_release);
  }
}

ShmTransportRegion::ShmTransportRegion(int fd,
                                       void* mapping,
                                       std::size_t mapped_bytes,
                                       std::string shm_name,
                                       ShmTransportOptions options,
                                       detail::ShmTransportHeader* header,
                                       detail::ShmSlotDescriptor* descriptors) noexcept
    : fd_(fd), mapping_(mapping), mapped_bytes_(mapped_bytes), shm_name_(kj::mv(shm_name)),
      options_(options), header_(header), descriptors_(descriptors) {}

ShmTransportRegion::~ShmTransportRegion() {
  reset();
}

ShmTransportRegion::ShmTransportRegion(ShmTransportRegion&& other) noexcept {
  *this = kj::mv(other);
}

ShmTransportRegion& ShmTransportRegion::operator=(ShmTransportRegion&& other) noexcept {
  if (this != &other) {
    reset();
    fd_ = std::exchange(other.fd_, -1);
    mapping_ = std::exchange(other.mapping_, nullptr);
    mapped_bytes_ = std::exchange(other.mapped_bytes_, 0);
    shm_name_ = kj::mv(other.shm_name_);
    options_ = other.options_;
    header_ = std::exchange(other.header_, nullptr);
    descriptors_ = std::exchange(other.descriptors_, nullptr);
  }
  return *this;
}

std::expected<ShmTransportRegion, std::string>
ShmTransportRegion::create(std::string_view logical_name, const ShmTransportOptions& options) {
  auto normalized_name = normalize_shm_name(logical_name);
  if (!normalized_name) {
    return std::unexpected(normalized_name.error());
  }

  auto layout = compute_transport_layout(options);
  if (!layout) {
    return std::unexpected(layout.error());
  }

  const int fd = ::shm_open(normalized_name->c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return std::unexpected(system_error_message("shm_open(create) failed", errno));
  }

  void* mapping = nullptr;
  std::size_t mapped_bytes = 0;
  auto cleanup = [&]() noexcept {
    close_mapping(mapping, mapped_bytes);
    int owned_fd = fd;
    close_fd(owned_fd);
    static_cast<void>(::shm_unlink(normalized_name->c_str()));
  };

  if (::ftruncate(fd, static_cast<off_t>(layout->total_bytes)) != 0) {
    const auto error = system_error_message("ftruncate() failed for shared memory", errno);
    cleanup();
    return std::unexpected(error);
  }

  mapping = ::mmap(nullptr, layout->total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    mapping = nullptr;
    const auto error = system_error_message("mmap() failed for shared memory", errno);
    cleanup();
    return std::unexpected(error);
  }

  mapped_bytes = layout->total_bytes;
  std::memset(mapping, 0, mapped_bytes);

  auto* base = reinterpret_cast<std::byte*>(mapping);
  auto* header = new (mapping) detail::ShmTransportHeader;
  header->magic = kShmMagic;
  header->version = kShmVersion;
  header->slot_count = static_cast<std::uint32_t>(options.slot_count);
  header->request_ring_capacity = static_cast<std::uint32_t>(options.request_ring_capacity);
  header->response_ring_capacity = static_cast<std::uint32_t>(options.response_ring_capacity);
  header->request_max_message_bytes = static_cast<std::uint32_t>(options.request_max_message_bytes);
  header->response_max_message_bytes =
      static_cast<std::uint32_t>(options.response_max_message_bytes);
  header->total_mapped_bytes = layout->total_bytes;

  auto* descriptors =
      reinterpret_cast<detail::ShmSlotDescriptor*>(base + layout->descriptors_offset);
  std::copy(layout->descriptors.begin(), layout->descriptors.end(), descriptors);

  for (const auto& descriptor : layout->descriptors) {
    initialize_ring(base + descriptor.request_ring_offset,
                    RingInitialization{
                        .capacity = options.request_ring_capacity,
                        .max_message_bytes = options.request_max_message_bytes,
                        .slot_stride_bytes = layout->request_slot_stride_bytes,
                    });
    initialize_ring(base + descriptor.response_ring_offset,
                    RingInitialization{
                        .capacity = options.response_ring_capacity,
                        .max_message_bytes = options.response_max_message_bytes,
                        .slot_stride_bytes = layout->response_slot_stride_bytes,
                    });
  }

  return ShmTransportRegion{
      fd,
      mapping,
      mapped_bytes,
      *normalized_name,
      options,
      header,
      descriptors,
  };
}

std::expected<ShmTransportRegion, std::string>
ShmTransportRegion::open(std::string_view logical_name) {
  auto normalized_name = normalize_shm_name(logical_name);
  if (!normalized_name) {
    return std::unexpected(normalized_name.error());
  }

  const int fd = ::shm_open(normalized_name->c_str(), O_RDWR, 0600);
  if (fd < 0) {
    return std::unexpected(system_error_message("shm_open(open) failed", errno));
  }

  struct stat metadata{};
  if (::fstat(fd, &metadata) != 0) {
    const auto error = system_error_message("fstat() failed for shared memory", errno);
    int owned_fd = fd;
    close_fd(owned_fd);
    return std::unexpected(error);
  }

  if (metadata.st_size <= 0) {
    int owned_fd = fd;
    close_fd(owned_fd);
    return std::unexpected("shared-memory object is empty");
  }

  void* mapping = ::mmap(nullptr, metadata.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    mapping = nullptr;
    const auto error = system_error_message("mmap() failed for shared memory", errno);
    int owned_fd = fd;
    close_fd(owned_fd);
    return std::unexpected(error);
  }

  auto* header = reinterpret_cast<detail::ShmTransportHeader*>(mapping);
  if (header->magic != kShmMagic) {
    auto* mapped = mapping;
    std::size_t mapped_bytes = metadata.st_size;
    close_mapping(mapped, mapped_bytes);
    int owned_fd = fd;
    close_fd(owned_fd);
    return std::unexpected("shared-memory header magic does not match rpc-bench transport");
  }

  if (header->version != kShmVersion) {
    auto* mapped = mapping;
    std::size_t mapped_bytes = metadata.st_size;
    close_mapping(mapped, mapped_bytes);
    int owned_fd = fd;
    close_fd(owned_fd);
    return std::unexpected(
        std::format("unsupported shared-memory transport version {}", header->version));
  }

  const ShmTransportOptions options{
      .slot_count = header->slot_count,
      .request_ring_capacity = header->request_ring_capacity,
      .response_ring_capacity = header->response_ring_capacity,
      .request_max_message_bytes = header->request_max_message_bytes,
      .response_max_message_bytes = header->response_max_message_bytes,
  };

  auto layout = compute_transport_layout(options);
  if (!layout) {
    auto* mapped = mapping;
    std::size_t mapped_bytes = metadata.st_size;
    close_mapping(mapped, mapped_bytes);
    int owned_fd = fd;
    close_fd(owned_fd);
    return std::unexpected(layout.error());
  }

  if (header->total_mapped_bytes < layout->total_bytes ||
      static_cast<std::size_t>(metadata.st_size) < layout->total_bytes) {
    auto* mapped = mapping;
    std::size_t mapped_bytes = metadata.st_size;
    close_mapping(mapped, mapped_bytes);
    int owned_fd = fd;
    close_fd(owned_fd);
    return std::unexpected("shared-memory object size does not match recorded transport layout");
  }

  auto* base = reinterpret_cast<std::byte*>(mapping);
  auto* descriptors =
      reinterpret_cast<detail::ShmSlotDescriptor*>(base + layout->descriptors_offset);
  for (std::size_t index = 0; index < layout->descriptors.size(); ++index) {
    if (descriptors[index].request_ring_offset != layout->descriptors[index].request_ring_offset ||
        descriptors[index].response_ring_offset !=
            layout->descriptors[index].response_ring_offset) {
      auto* mapped = mapping;
      std::size_t mapped_bytes = metadata.st_size;
      close_mapping(mapped, mapped_bytes);
      int owned_fd = fd;
      close_fd(owned_fd);
      return std::unexpected("shared-memory slot descriptor table does not match expected layout");
    }
  }

  return ShmTransportRegion{
      fd,
      mapping,
      static_cast<std::size_t>(metadata.st_size),
      *normalized_name,
      options,
      header,
      descriptors,
  };
}

const ShmTransportOptions& ShmTransportRegion::options() const {
  return options_;
}

std::string_view ShmTransportRegion::shared_memory_name() const {
  return shm_name_;
}

std::size_t ShmTransportRegion::mapped_bytes() const {
  return mapped_bytes_;
}

ShmSlotRings ShmTransportRegion::slot(std::size_t slot_index) const {
  KJ_REQUIRE(descriptors_ != nullptr, "shared-memory transport region is not initialized");
  KJ_REQUIRE(slot_index < options_.slot_count, "shared-memory slot index out of range", slot_index);

  const auto& descriptor = descriptors_[slot_index];
  return ShmSlotRings{
      .request = ring_from_offset(descriptor.request_ring_offset),
      .response = ring_from_offset(descriptor.response_ring_offset),
  };
}

ShmEndpointRings ShmTransportRegion::benchmark_endpoint(std::size_t slot_index) const {
  const auto rings = slot(slot_index);
  return ShmEndpointRings{
      .inbound = rings.response,
      .outbound = rings.request,
  };
}

ShmEndpointRings ShmTransportRegion::server_endpoint(std::size_t slot_index) const {
  const auto rings = slot(slot_index);
  return ShmEndpointRings{
      .inbound = rings.request,
      .outbound = rings.response,
  };
}

std::expected<void, std::string> ShmTransportRegion::unlink() const {
  if (shm_name_.empty()) {
    return std::unexpected("shared-memory region does not have a name");
  }
  if (::shm_unlink(shm_name_.c_str()) != 0 && errno != ENOENT) {
    return std::unexpected(system_error_message("shm_unlink() failed", errno));
  }
  return {};
}

void ShmTransportRegion::reset() noexcept {
  header_ = nullptr;
  descriptors_ = nullptr;
  close_mapping(mapping_, mapped_bytes_);
  close_fd(fd_);
  shm_name_.clear();
  options_ = {};
}

ShmMessageRingView ShmTransportRegion::ring_from_offset(std::uint64_t offset) const {
  KJ_REQUIRE(mapping_ != nullptr, "shared-memory transport region is not mapped");
  KJ_REQUIRE(offset + sizeof(detail::ShmRingHeader) <= mapped_bytes_,
             "shared-memory ring offset is out of range",
             offset,
             mapped_bytes_);

  auto* base = reinterpret_cast<std::byte*>(mapping_);
  auto* header = reinterpret_cast<detail::ShmRingHeader*>(base + offset);
  auto* slots = base + offset + sizeof(detail::ShmRingHeader);
  return ShmMessageRingView(header, slots);
}

kj::Promise<void> SameThreadWakeListener::wait() {
  if (closed_) {
    const std::string error =
        close_reason_.empty() ? std::string("same-thread wake listener closed") : close_reason_;
    return kj::Promise<void>(KJ_EXCEPTION(DISCONNECTED, error));
  }
  if (pending_signals_ > 0) {
    --pending_signals_;
    return kj::READY_NOW;
  }
  if (waiter_.get() != nullptr) {
    return kj::Promise<void>(
        KJ_EXCEPTION(FAILED, "same-thread wake listener already has a waiter"));
  }

  auto pair = kj::newPromiseAndFulfiller<void>();
  waiter_ = kj::mv(pair.fulfiller);
  return kj::mv(pair.promise);
}

void SameThreadWakeListener::close(std::string_view reason) {
  if (closed_) {
    return;
  }

  closed_ = true;
  pending_signals_ = 0;
  close_reason_ = std::string(reason);

  auto waiter = kj::mv(waiter_);
  if (waiter.get() != nullptr) {
    const std::string error =
        close_reason_.empty() ? std::string("same-thread wake listener closed") : close_reason_;
    waiter->reject(KJ_EXCEPTION(DISCONNECTED, error));
  }
}

void SameThreadWakeListener::signal() {
  if (closed_) {
    return;
  }
  auto waiter = kj::mv(waiter_);
  if (waiter.get() != nullptr) {
    waiter->fulfill();
    return;
  }
  ++pending_signals_;
}

kj::Promise<void> CrossThreadWakeListener::wait() {
  const std::scoped_lock lock(mutex_);
  if (closed_) {
    const std::string error =
        close_reason_.empty() ? std::string("cross-thread wake listener closed") : close_reason_;
    return kj::Promise<void>(KJ_EXCEPTION(DISCONNECTED, error));
  }
  if (pending_signals_ > 0) {
    --pending_signals_;
    return kj::READY_NOW;
  }
  if (waiter_.get() != nullptr) {
    return kj::Promise<void>(
        KJ_EXCEPTION(FAILED, "cross-thread wake listener already has a waiter"));
  }

  auto pair = kj::newPromiseAndCrossThreadFulfiller<void>();
  waiter_ = kj::mv(pair.fulfiller);
  return kj::mv(pair.promise);
}

void CrossThreadWakeListener::close(std::string_view reason) {
  kj::Own<kj::CrossThreadPromiseFulfiller<void>> waiter;
  {
    const std::scoped_lock lock(mutex_);
    if (closed_) {
      return;
    }

    closed_ = true;
    pending_signals_ = 0;
    close_reason_ = std::string(reason);
    waiter = kj::mv(waiter_);
  }

  if (waiter.get() != nullptr) {
    const std::string error =
        close_reason_.empty() ? std::string("cross-thread wake listener closed") : close_reason_;
    waiter->reject(KJ_EXCEPTION(DISCONNECTED, error));
  }
}

void CrossThreadWakeListener::signal() {
  kj::Own<kj::CrossThreadPromiseFulfiller<void>> waiter;
  {
    const std::scoped_lock lock(mutex_);
    if (closed_) {
      return;
    }

    waiter = kj::mv(waiter_);
    if (waiter.get() == nullptr) {
      ++pending_signals_;
      return;
    }
  }

  waiter->fulfill();
}

std::expected<void, std::string> ControlMessageSender::send_attach(std::size_t slot_count) {
  return send(ControlMessageKind::attach, slot_count);
}

std::expected<void, std::string> ControlMessageSender::send_init(std::size_t slot_index) {
  return send(ControlMessageKind::init, slot_index);
}

std::expected<void, std::string> ControlMessageSender::send_wake(std::size_t slot_index) {
  return send(ControlMessageKind::wake, slot_index);
}

HybridControlSocket::HybridControlSocket(int fd) noexcept : fd_(fd) {
#ifdef SO_NOSIGPIPE
  if (fd_ >= 0) {
    const int enabled = 1;
    static_cast<void>(::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)));
  }
#endif
}

HybridControlSocket::~HybridControlSocket() {
  close_fd(fd_);
}

int HybridControlSocket::fd() const noexcept {
  return fd_;
}

bool HybridControlSocket::valid() const noexcept {
  return fd_ >= 0;
}

std::expected<void, std::string> HybridControlSocket::send(ControlMessageKind kind,
                                                           std::size_t slot_index) {
  if (!valid()) {
    return std::unexpected("control socket is not open");
  }

  auto message = make_control_message(kind, slot_index);
  if (!message) {
    return std::unexpected(message.error());
  }

  const auto bytes = std::as_bytes(std::span(message.operator->(), 1));
  const std::scoped_lock lock(send_mutex_);
  return send_all(fd_, bytes);
}

std::expected<void, std::string> HybridControlSocket::shutdown() const {
  if (!valid()) {
    return {};
  }
  if (::shutdown(fd_, SHUT_RDWR) != 0 && errno != ENOTCONN) {
    return std::unexpected(system_error_message("shutdown() failed for control socket", errno));
  }
  return {};
}

ParentWakeBroker::ParentWakeBroker(int fd, std::span<CrossThreadWakeListener*> listeners)
    : socket_(fd), listeners_(listeners.begin(), listeners.end()) {}

ParentWakeBroker::~ParentWakeBroker() noexcept {
  stop();
}

std::expected<void, std::string> ParentWakeBroker::start() {
  if (started_) {
    return std::unexpected("parent wake broker was already started");
  }

  try {
    stopping_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this]() { run(); });
    started_ = true;
    return {};
  } catch (const std::system_error& error) {
    return std::unexpected(
        std::format("failed to start parent wake broker thread: {}", error.what()));
  }
}

void ParentWakeBroker::stop() noexcept {
  if (!started_) {
    return;
  }

  stopping_.store(true, std::memory_order_relaxed);
  if (socket_.fd() >= 0) {
    static_cast<void>(::shutdown(socket_.fd(), SHUT_RDWR));
  }

  if (thread_.joinable()) {
    thread_.join();
  }

  started_ = false;
}

std::expected<void, std::string> ParentWakeBroker::send(ControlMessageKind kind,
                                                        std::size_t slot_index) {
  return socket_.send(kind, slot_index);
}

std::optional<std::string> ParentWakeBroker::background_error() const {
  const std::scoped_lock lock(error_mutex_);
  return background_error_;
}

void ParentWakeBroker::run() {
  while (!stopping_.load(std::memory_order_relaxed)) {
    ControlMessage message{};
    auto read_result = read_exact(
        socket_.fd(), std::span(reinterpret_cast<std::byte*>(&message), sizeof(message)));
    if (!read_result) {
      if (stopping_.load(std::memory_order_relaxed)) {
        close_listeners("parent wake broker stopped");
      } else {
        record_background_error(read_result.error());
        close_listeners(read_result.error());
      }
      return;
    }

    auto signal_result = signal_control_message(std::span(listeners_), message);
    if (!signal_result) {
      record_background_error(signal_result.error());
      close_listeners(signal_result.error());
      return;
    }
  }

  close_listeners("parent wake broker stopped");
}

void ParentWakeBroker::record_background_error(std::string error) {
  const std::scoped_lock lock(error_mutex_);
  if (!background_error_) {
    background_error_ = kj::mv(error);
  }
}

void ParentWakeBroker::close_listeners(std::string_view reason) {
  for (auto* listener : listeners_) {
    if (listener != nullptr) {
      listener->close(reason);
    }
  }
}

ServerControlDispatcher::ServerControlDispatcher(kj::AsyncIoStream& stream,
                                                 std::span<SameThreadWakeListener*> listeners)
    : stream_(stream), listeners_(listeners.begin(), listeners.end()),
      init_seen_(listeners.size(), 0) {
  auto pair = kj::newPromiseAndFulfiller<void>();
  init_fulfiller_ = kj::mv(pair.fulfiller);
  init_ready_ = kj::mv(pair.promise).fork();

  if (listeners_.empty() && init_fulfiller_.get() != nullptr) {
    auto fulfiller = kj::mv(init_fulfiller_);
    fulfiller->fulfill();
  }
}

kj::Promise<void> ServerControlDispatcher::run() {
  if (started_) {
    return kj::Promise<void>(KJ_EXCEPTION(FAILED, "server control dispatcher already started"));
  }
  started_ = true;
  return pump_one();
}

kj::Promise<void> ServerControlDispatcher::initialized() {
  return init_ready_.addBranch();
}

std::size_t ServerControlDispatcher::active_slot_count() const {
  return active_slot_count_;
}

kj::Promise<void> ServerControlDispatcher::pump_one() {
  return stream_.read(&read_buffer_, sizeof(read_buffer_))
      .then(
          [this]() -> kj::Promise<void> {
            auto handled = handle_message(read_buffer_);
            if (!handled) {
              reject_init(handled.error());
              close_listeners(handled.error());
              return kj::Promise<void>(KJ_EXCEPTION(FAILED, handled.error()));
            }
            return pump_one();
          },
          [this](kj::Exception&& exception) -> kj::Promise<void> {
            const std::string error = kj::str(exception).cStr();
            reject_init(error);
            close_listeners(error);
            return kj::Promise<void>(kj::mv(exception));
          });
}

std::expected<void, std::string>
ServerControlDispatcher::handle_message(const ControlMessage& message) {
  if (message.kind == ControlMessageKind::attach) {
    if (attach_seen_) {
      return std::unexpected("shared-memory transport received duplicate attach message");
    }

    const auto announced_slots = static_cast<std::size_t>(message.slot_index);
    if (announced_slots == 0) {
      return std::unexpected("shared-memory attach message must announce at least one slot");
    }
    if (announced_slots > listeners_.size()) {
      return std::unexpected(
          std::format("shared-memory attach announced {} slots but the server only provisioned {}",
                      announced_slots,
                      listeners_.size()));
    }

    attach_seen_ = true;
    active_slot_count_ = announced_slots;
    if (init_count_ == active_slot_count_ && init_fulfiller_.get() != nullptr) {
      auto fulfiller = kj::mv(init_fulfiller_);
      fulfiller->fulfill();
    }
    return {};
  }

  if (!attach_seen_) {
    return std::unexpected("shared-memory control message arrived before the attach handshake");
  }

  const auto slot_index = static_cast<std::size_t>(message.slot_index);
  if (slot_index >= active_slot_count_) {
    return std::unexpected(std::format("control message referenced inactive slot {}", slot_index));
  }

  auto signal_result =
      signal_control_message(std::span(listeners_.data(), active_slot_count_), message);
  if (!signal_result) {
    return std::unexpected(signal_result.error());
  }

  if (message.kind == ControlMessageKind::init) {
    const auto slot_index = static_cast<std::size_t>(message.slot_index);
    if (init_seen_[slot_index] == 0) {
      init_seen_[slot_index] = 1;
      ++init_count_;
      if (init_count_ == active_slot_count_ && init_fulfiller_.get() != nullptr) {
        auto fulfiller = kj::mv(init_fulfiller_);
        fulfiller->fulfill();
      }
    }
  }

  return {};
}

void ServerControlDispatcher::close_listeners(std::string_view reason) {
  for (auto* listener : listeners_) {
    if (listener != nullptr) {
      listener->close(reason);
    }
  }
}

void ServerControlDispatcher::reject_init(std::string_view reason) {
  if (init_fulfiller_.get() != nullptr) {
    auto fulfiller = kj::mv(init_fulfiller_);
    const std::string error =
        reason.empty() ? std::string("server control dispatcher stopped") : std::string(reason);
    fulfiller->reject(KJ_EXCEPTION(DISCONNECTED, error));
  }
}

ShmMessageStream::ShmMessageStream(std::size_t slot_index,
                                   ShmEndpointRings rings,
                                   WakeListener& inbound_wake,
                                   ControlMessageSender& outbound_wake_sender)
    : slot_index_(slot_index), inbound_ring_(rings.inbound), outbound_ring_(rings.outbound),
      inbound_wake_(&inbound_wake), outbound_wake_sender_(&outbound_wake_sender) {
  KJ_REQUIRE(inbound_ring_.valid(), "shared-memory message stream requires an inbound ring");
  KJ_REQUIRE(outbound_ring_.valid(), "shared-memory message stream requires an outbound ring");
}

kj::Promise<kj::Maybe<capnp::MessageReaderAndFds>>
ShmMessageStream::tryReadMessage(kj::ArrayPtr<kj::AutoCloseFd> fd_space,
                                 capnp::ReaderOptions options,
                                 kj::ArrayPtr<capnp::word> scratch_space) {
  return wait_for_message(fd_space, options, scratch_space);
}

kj::Promise<kj::Maybe<capnp::MessageReaderAndFds>>
ShmMessageStream::wait_for_message(kj::ArrayPtr<kj::AutoCloseFd> fd_space,
                                   capnp::ReaderOptions options,
                                   kj::ArrayPtr<capnp::word> scratch_space) {
  static_cast<void>(fd_space);
  static_cast<void>(scratch_space);

  auto maybe_message = inbound_ring_.try_pop_bytes();
  if (!maybe_message) {
    return kj::Promise<kj::Maybe<capnp::MessageReaderAndFds>>(
        KJ_EXCEPTION(FAILED, maybe_message.error()));
  }

  if (*maybe_message) {
    auto decoded = decode_owned_message(**maybe_message, options);
    if (!decoded) {
      return kj::Promise<kj::Maybe<capnp::MessageReaderAndFds>>(
          KJ_EXCEPTION(FAILED, decoded.error()));
    }
    return kj::Maybe<capnp::MessageReaderAndFds>(kj::mv(*decoded));
  }

  if (inbound_ring_.drained_and_closed()) {
    return kj::Maybe<capnp::MessageReaderAndFds>(nullptr);
  }

  if (read_wait_active_) {
    return kj::Promise<kj::Maybe<capnp::MessageReaderAndFds>>(
        KJ_EXCEPTION(FAILED, "shared-memory message stream already has a pending read"));
  }

  read_wait_active_ = true;
  return inbound_wake_->wait().then(
      [this, fd_space, options, scratch_space]()
          -> kj::Promise<kj::Maybe<capnp::MessageReaderAndFds>> {
        read_wait_active_ = false;
        return wait_for_message(fd_space, options, scratch_space);
      },
      [this](kj::Exception&& exception) -> kj::Promise<kj::Maybe<capnp::MessageReaderAndFds>> {
        read_wait_active_ = false;
        return kj::Promise<kj::Maybe<capnp::MessageReaderAndFds>>(kj::mv(exception));
      });
}

kj::Promise<void>
ShmMessageStream::writeMessage(kj::ArrayPtr<const int> fds,
                               kj::ArrayPtr<const kj::ArrayPtr<const capnp::word>> segments) {
  if (fds.size() != 0) {
    return kj::Promise<void>(
        KJ_EXCEPTION(UNIMPLEMENTED, "shared-memory transport does not support FD passing"));
  }
  if (ended_) {
    return kj::Promise<void>(
        KJ_EXCEPTION(DISCONNECTED, "shared-memory message stream write end is closed"));
  }

  auto flat = capnp::messageToFlatArray(segments);
  auto bytes = flat.asBytes();
  auto payload = std::span(reinterpret_cast<const std::byte*>(bytes.begin()), bytes.size());
  auto pushed = outbound_ring_.push_bytes(payload);
  if (!pushed) {
    return kj::Promise<void>(KJ_EXCEPTION(OVERLOADED, pushed.error()));
  }

  auto notified = notify_peer(ControlMessageKind::wake);
  if (!notified) {
    return kj::Promise<void>(KJ_EXCEPTION(FAILED, notified.error()));
  }

  return kj::READY_NOW;
}

kj::Promise<void> ShmMessageStream::writeMessages(
    kj::ArrayPtr<kj::ArrayPtr<const kj::ArrayPtr<const capnp::word>>> messages) {
  if (ended_) {
    return kj::Promise<void>(
        KJ_EXCEPTION(DISCONNECTED, "shared-memory message stream write end is closed"));
  }
  if (messages.size() == 0) {
    return kj::READY_NOW;
  }
  if (messages.size() > outbound_ring_.available_slots()) {
    return kj::Promise<void>(KJ_EXCEPTION(OVERLOADED,
                                          "shared-memory ring does not have enough free slots "
                                          "for the requested batch"));
  }

  for (const auto& message : messages) {
    auto flat = capnp::messageToFlatArray(message);
    auto bytes = flat.asBytes();
    auto payload = std::span(reinterpret_cast<const std::byte*>(bytes.begin()), bytes.size());
    auto pushed = outbound_ring_.push_bytes(payload);
    if (!pushed) {
      return kj::Promise<void>(KJ_EXCEPTION(OVERLOADED, pushed.error()));
    }
  }

  auto notified = notify_peer(ControlMessageKind::wake);
  if (!notified) {
    return kj::Promise<void>(KJ_EXCEPTION(FAILED, notified.error()));
  }

  return kj::READY_NOW;
}

kj::Maybe<int> ShmMessageStream::getSendBufferSize() {
  return static_cast<int>(
      std::min<std::size_t>(outbound_ring_.buffer_bytes(), std::numeric_limits<int>::max()));
}

kj::Promise<void> ShmMessageStream::end() {
  if (ended_) {
    return kj::READY_NOW;
  }

  ended_ = true;
  outbound_ring_.close_writer();
  auto notified = notify_peer(ControlMessageKind::wake);
  if (!notified) {
    return kj::Promise<void>(KJ_EXCEPTION(FAILED, notified.error()));
  }
  return kj::READY_NOW;
}

std::expected<void, std::string> ShmMessageStream::notify_peer(ControlMessageKind kind) const {
  KJ_REQUIRE(outbound_wake_sender_ != nullptr, "shared-memory message stream has no wake sender");
  return outbound_wake_sender_->send(kind, slot_index_);
}

} // namespace rpcbench
