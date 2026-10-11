#include "xenon/xbox/xex_dispatcher_header.hpp"

#include <mutex>
#include <unordered_map>

#include "xenon/kernel/event.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/semaphore.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/fault.hpp"

namespace xenon::xbox {
namespace {

// Xenon's own marker for "this X_DISPATCH_HEADER has already been
// associated with a host object" - purely a host-side implementation
// detail never visible to or interpreted by guest code (see
// xex_dispatcher_header.hpp's doc comment), so its exact value only needs
// to be internally consistent, not match any real Xbox 360 kernel constant.
// Deliberately a different value from xenia-project/xenia's own equivalent
// marker (independently chosen, not copied).
constexpr std::uint32_t kDispatcherStashSignature = 0x584E4B31u;  // "XNK1"

// The guest header's SignalState for an event or semaphore resolved from it.
// Holds the process's address space so a signal after the resolving call
// still has memory to write to. A header in memory the guest has since freed
// or unmapped faults; that access is dropped, as the guest no longer owns the
// object there.
class GuestDispatchHeaderMirror final : public kernel::SignalStateMirror {
 public:
  GuestDispatchHeaderMirror(std::shared_ptr<memory::AddressSpace> memory,
                            cpu::GuestAddress header_address)
      : memory_(std::move(memory)),
        signal_state_(header_address + DispatchHeaderLayout::kSignalStateOffset) {}

  std::int32_t load() noexcept override {
    try {
      return static_cast<std::int32_t>(memory_->read32_be(signal_state_));
    } catch (const memory::MemoryFault&) {
      return last_stored_;
    }
  }

  void store(std::int32_t signal_state) noexcept override {
    last_stored_ = signal_state;
    try {
      memory_->write32_be(signal_state_, static_cast<std::uint32_t>(signal_state));
    } catch (const memory::MemoryFault&) {
    }
  }

 private:
  std::shared_ptr<memory::AddressSpace> memory_;
  cpu::GuestAddress signal_state_;
  std::int32_t last_stored_{};
};

// Serialises every stash of a guest header (see resolve_dispatcher_object()).
std::mutex& resolve_mutex() {
  static std::mutex mutex;
  return mutex;
}

// Headers published for handle-created objects, by object id (ids are never
// reused). Guarded by resolve_mutex().
std::unordered_map<std::uint64_t, cpu::GuestAddress>& published_headers() {
  static std::unordered_map<std::uint64_t, cpu::GuestAddress> headers;
  return headers;
}

kernel::GuestStateSync reconcile(kernel::KernelObject& object) {
  switch (object.type()) {
    case kernel::ObjectType::Event:
      return static_cast<kernel::KernelEvent&>(object).reconcile_with_guest_state();
    case kernel::ObjectType::Semaphore:
      return static_cast<kernel::KernelSemaphore&>(object).reconcile_with_guest_state();
    default:
      return kernel::GuestStateSync::Unchanged;
  }
}

}  // namespace

std::shared_ptr<kernel::KernelObject> resolve_dispatcher_object(
    kernel::KernelProcess& process, cpu::MemoryPort& memory, cpu::GuestAddress header_address,
    std::string* error, kernel::GuestStateSync* sync) {
  if (sync) *sync = kernel::GuestStateSync::Unchanged;
  if (header_address == 0u) {
    if (error) *error = "null X_DISPATCH_HEADER pointer";
    return nullptr;
  }

  // Resolution is check-then-create-then-stash. Two guest threads that first touch
  // the same embedded KEVENT/KSEMAPHORE at the same moment (a waiter and the
  // thread that signals it - the normal case for a lock or a work queue) would
  // each see "not stashed yet" and each create their own host object, so the
  // signal lands on a different object than the one being waited on and the
  // wakeup is lost. The stash is also two guest words (signature, then handle),
  // which a concurrent reader could observe half-written. One process-wide mutex
  // makes the whole resolution atomic; it is held only for this short section
  // (no wait ever happens under it).
  std::scoped_lock resolve_lock(resolve_mutex());

  const auto flink = memory.read32_be(header_address + DispatchHeaderLayout::kWaitListFlinkOffset);
  if (flink == kDispatcherStashSignature) {
    const auto handle = memory.read32_be(header_address + DispatchHeaderLayout::kWaitListBlinkOffset);
    kernel::HandleView view{};
    if (process.handle_table().lookup(static_cast<kernel::Handle>(handle), view) ==
        kernel::KernelIoCode::Success) {
      const auto synced = reconcile(*view.object);
      if (sync) *sync = synced;
      return view.object;
    }
    // Stashed handle is stale (the host object was destroyed and its handle
    // possibly reused for something else) - fall through and recreate,
    // rather than trusting a marker that no longer resolves to anything.
  }

  const auto type_byte = memory.read8(header_address + DispatchHeaderLayout::kTypeOffset);
  std::shared_ptr<kernel::KernelObject> object;

  switch (type_byte) {
    case static_cast<std::uint8_t>(DispatchObjectType::EventNotification):
    case static_cast<std::uint8_t>(DispatchObjectType::EventSynchronization): {
      const bool manual_reset =
          type_byte == static_cast<std::uint8_t>(DispatchObjectType::EventNotification);
      const auto signal_state =
          memory.read32_be(header_address + DispatchHeaderLayout::kSignalStateOffset);
      auto event = std::make_shared<kernel::KernelEvent>(manual_reset, signal_state != 0u);
      event->attach_signal_state_mirror(std::make_unique<GuestDispatchHeaderMirror>(
          process.memory().shared_address_space(), header_address));
      object = std::move(event);
      break;
    }
    case static_cast<std::uint8_t>(DispatchObjectType::Semaphore): {
      const auto count = static_cast<std::int32_t>(
          memory.read32_be(header_address + DispatchHeaderLayout::kSignalStateOffset));
      const auto limit = static_cast<std::int32_t>(
          memory.read32_be(header_address + DispatchHeaderLayout::kSemaphoreLimitOffset));
      if (limit <= 0 || count < 0 || count > limit) {
        if (error) {
          *error = "X_KSEMAPHORE at 0x" + std::to_string(header_address) +
                   " has an invalid count/limit (count=" + std::to_string(count) +
                   " limit=" + std::to_string(limit) + ")";
        }
        return nullptr;
      }
      auto semaphore = std::make_shared<kernel::KernelSemaphore>(count, limit);
      semaphore->attach_signal_state_mirror(std::make_unique<GuestDispatchHeaderMirror>(
          process.memory().shared_address_space(), header_address));
      object = std::move(semaphore);
      break;
    }
    default:
      if (error) {
        *error = "unsupported X_DISPATCH_HEADER type " + std::to_string(static_cast<int>(type_byte)) +
                 " at 0x" + std::to_string(header_address) +
                 " (only Event/Semaphore are modeled - see docs/kernel/THREADING_V2.md)";
      }
      return nullptr;
  }

  kernel::Handle handle{};
  if (process.handle_table().insert(object, /*granted_access=*/0xFFFFFFFFu, kernel::HandleFlags::None,
                                    handle) != kernel::KernelIoCode::Success) {
    if (error) *error = "failed to publish a handle for the resolved dispatcher object";
    return nullptr;
  }

  memory.write32_be(header_address + DispatchHeaderLayout::kWaitListFlinkOffset,
                    kDispatcherStashSignature);
  memory.write32_be(header_address + DispatchHeaderLayout::kWaitListBlinkOffset, handle);
  return object;
}

cpu::GuestAddress publish_dispatcher_header(kernel::KernelProcess& process,
                                            const std::shared_ptr<kernel::KernelObject>& object) {
  if (!object) return 0u;
  const auto type = object->type();
  if (type != kernel::ObjectType::Event && type != kernel::ObjectType::Semaphore) return 0u;

  std::scoped_lock resolve_lock(resolve_mutex());
  auto& published = published_headers();
  if (const auto found = published.find(object->object_id()); found != published.end()) {
    return found->second;
  }

  const bool is_event = type == kernel::ObjectType::Event;
  const auto address = process.pool().allocate(
      is_event ? DispatchHeaderLayout::kSize : DispatchHeaderLayout::kSemaphoreLimitOffset + 4u,
      is_event ? 0x4576656Eu /* "Even" */ : 0x53656D61u /* "Sema" */);
  if (address == 0u) return 0u;

  kernel::Handle handle{};
  if (process.handle_table().insert(object, /*granted_access=*/0xFFFFFFFFu,
                                    kernel::HandleFlags::None, handle) !=
      kernel::KernelIoCode::Success) {
    static_cast<void>(process.pool().free(address));
    return 0u;
  }

  auto& memory = process.memory().address_space();
  auto mirror = std::make_unique<GuestDispatchHeaderMirror>(
      process.memory().shared_address_space(), address);
  if (is_event) {
    auto& event = static_cast<kernel::KernelEvent&>(*object);
    initialize_dispatch_header(memory, address,
                               event.manual_reset() ? DispatchObjectType::EventNotification
                                                    : DispatchObjectType::EventSynchronization,
                               0u);
    event.attach_signal_state_mirror(std::move(mirror));  // writes the current state
  } else {
    auto& semaphore = static_cast<kernel::KernelSemaphore&>(*object);
    initialize_dispatch_header(memory, address, DispatchObjectType::Semaphore, 0u,
                               semaphore.maximum_count());
    semaphore.attach_signal_state_mirror(std::move(mirror));
  }
  memory.write32_be(address + DispatchHeaderLayout::kWaitListFlinkOffset,
                    kDispatcherStashSignature);
  memory.write32_be(address + DispatchHeaderLayout::kWaitListBlinkOffset, handle);
  published.emplace(object->object_id(), address);
  return address;
}

void initialize_dispatch_header(cpu::MemoryPort& memory, cpu::GuestAddress header_address,
                                DispatchObjectType type, std::uint32_t signal_state,
                                std::optional<std::int32_t> semaphore_limit) {
  memory.write8(header_address + DispatchHeaderLayout::kTypeOffset, static_cast<std::uint8_t>(type));
  memory.write8(header_address + DispatchHeaderLayout::kTypeOffset + 1u, 0u);
  memory.write8(header_address + DispatchHeaderLayout::kTypeOffset + 2u, 0u);
  memory.write8(header_address + DispatchHeaderLayout::kTypeOffset + 3u, 0u);
  memory.write32_be(header_address + DispatchHeaderLayout::kSignalStateOffset, signal_state);
  // Clearing wait_list_flink/wait_list_blink invalidates any previously
  // stashed host-object association at this address - (re-)initializing a
  // kernel object always starts it fresh, matching real Xbox 360 semantics
  // and the verified reference behavior.
  memory.write32_be(header_address + DispatchHeaderLayout::kWaitListFlinkOffset, 0u);
  memory.write32_be(header_address + DispatchHeaderLayout::kWaitListBlinkOffset, 0u);
  if (semaphore_limit.has_value()) {
    memory.write32_be(header_address + DispatchHeaderLayout::kSemaphoreLimitOffset,
                      static_cast<std::uint32_t>(*semaphore_limit));
  }
}

}  // namespace xenon::xbox
