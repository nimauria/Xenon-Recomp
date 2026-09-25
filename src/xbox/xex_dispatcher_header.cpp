#include "xenon/xbox/xex_dispatcher_header.hpp"

#include "xenon/kernel/event.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/semaphore.hpp"

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

}  // namespace

std::shared_ptr<kernel::KernelObject> resolve_dispatcher_object(
    kernel::KernelProcess& process, cpu::MemoryPort& memory, cpu::GuestAddress header_address,
    std::string* error) {
  if (header_address == 0u) {
    if (error) *error = "null X_DISPATCH_HEADER pointer";
    return nullptr;
  }

  const auto flink = memory.read32_be(header_address + DispatchHeaderLayout::kWaitListFlinkOffset);
  if (flink == kDispatcherStashSignature) {
    const auto handle = memory.read32_be(header_address + DispatchHeaderLayout::kWaitListBlinkOffset);
    kernel::HandleView view{};
    if (process.handle_table().lookup(static_cast<kernel::Handle>(handle), view) ==
        kernel::KernelIoCode::Success) {
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
      object = std::make_shared<kernel::KernelEvent>(manual_reset, signal_state != 0u);
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
      object = std::make_shared<kernel::KernelSemaphore>(count, limit);
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
