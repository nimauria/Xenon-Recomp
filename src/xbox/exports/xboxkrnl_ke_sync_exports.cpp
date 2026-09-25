#include "xenon/xbox/xboxkrnl_ke_sync_exports.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/semaphore.hpp"
#include "xenon/kernel/wait.hpp"
#include "xenon/kernel/xbox_io.hpp"
#include "xenon/logging/logger.hpp"
#include "xenon/xbox/xbox_time_convert.hpp"
#include "xenon/xbox/xex_dispatcher_header.hpp"

namespace xenon::xbox {
namespace {

using xenon::core::ExportCallContext;
using xenon::kernel::KernelObject;
using xenon::kernel::ObjectType;

namespace status = xenon::kernel::xbox::status;

void log_resolution_failure(std::string_view function, const std::string& error) {
  logging::Logger::instance().log_if_enabled(logging::Level::Warning, "ke_sync", [&] {
    return std::string(function) + ": " + error;
  });
}

std::uint32_t to_status(kernel::WaitResult result, std::uint32_t signaled_index_as_status) {
  switch (result) {
    case kernel::WaitResult::Success: return signaled_index_as_status;
    case kernel::WaitResult::Timeout: return status::Timeout;
    case kernel::WaitResult::Abandoned: return 0x00000080u;  // STATUS_ABANDONED_WAIT_0
    case kernel::WaitResult::Failed: return status::Unsuccessful;
  }
  return status::Unsuccessful;
}

std::chrono::milliseconds read_timeout(ExportCallContext& context, cpu::GuestAddress timeout_ptr) {
  if (timeout_ptr == 0u) {
    return xbox_infinite_timeout();
  }
  const auto raw = static_cast<std::int64_t>(context.memory.read64_be(timeout_ptr));
  return xbox_timeout_to_relative_ms(raw);
}

}  // namespace

// KeInitializeEvent (ordinal 0x70)
// Guest ABI: r3 = X_DISPATCH_HEADER (X_KEVENT) guest pointer, r4 = event
// type (0 = NotificationEvent/manual-reset, 1 = SynchronizationEvent/
// auto-reset), r5 = initial state (BOOLEAN) -> void (no meaningful return).
bool ke_initialize_event_export(kernel::KernelProcess&, ExportCallContext& context) {
  const auto header_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto event_type = static_cast<std::uint32_t>(context.cpu.gpr[4]);
  const auto initial_state = static_cast<std::uint32_t>(context.cpu.gpr[5]);
  if (header_address == 0u) return true;

  const auto type = (event_type == 0u) ? DispatchObjectType::EventNotification
                                       : DispatchObjectType::EventSynchronization;
  initialize_dispatch_header(context.memory, header_address, type, initial_state);
  return true;
}

// KeInitializeSemaphore (ordinal 0x74)
// Guest ABI: r3 = X_DISPATCH_HEADER (X_KSEMAPHORE) guest pointer, r4 =
// initial count, r5 = maximum count (limit) -> void.
bool ke_initialize_semaphore_export(kernel::KernelProcess&, ExportCallContext& context) {
  const auto header_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto count = static_cast<std::uint32_t>(context.cpu.gpr[4]);
  const auto limit = static_cast<std::int32_t>(context.cpu.gpr[5]);
  if (header_address == 0u) return true;

  initialize_dispatch_header(context.memory, header_address, DispatchObjectType::Semaphore, count,
                             limit);
  return true;
}

// KeSetEvent (ordinal 0x9D)
// Guest ABI: r3 = event header pointer, r4 = priority increment (ignored -
// no APC/priority-boost semantics modeled), r5 = wait (ignored) ->
// r3 = previous signal state (LONG: 0 or 1) - real KeSetEvent's actual
// return convention, not an NTSTATUS.
bool ke_set_event_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto header_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);

  std::string error;
  auto object = resolve_dispatcher_object(process, context.memory, header_address, &error);
  if (!object || object->type() != ObjectType::Event) {
    log_resolution_failure("KeSetEvent", object ? "resolved object is not an Event" : error);
    context.cpu.gpr[3] = 0u;
    return true;
  }

  auto& event = static_cast<kernel::KernelEvent&>(*object);
  const std::uint32_t previous = event.signaled() ? 1u : 0u;
  event.set();
  context.cpu.gpr[3] = previous;
  return true;
}

// KeResetEvent (ordinal 0x8F)
// Guest ABI: r3 = event header pointer -> r3 = previous signal state.
bool ke_reset_event_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto header_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);

  std::string error;
  auto object = resolve_dispatcher_object(process, context.memory, header_address, &error);
  if (!object || object->type() != ObjectType::Event) {
    log_resolution_failure("KeResetEvent", object ? "resolved object is not an Event" : error);
    context.cpu.gpr[3] = 0u;
    return true;
  }

  auto& event = static_cast<kernel::KernelEvent&>(*object);
  const std::uint32_t previous = event.signaled() ? 1u : 0u;
  event.reset();
  context.cpu.gpr[3] = previous;
  return true;
}

// KeReleaseSemaphore (ordinal 0x88)
// Guest ABI: r3 = semaphore header pointer, r4 = priority increment
// (ignored), r5 = adjustment (release count), r6 = wait (ignored) ->
// r3 = previous count (LONG), matching real KeReleaseSemaphore semantics.
bool ke_release_semaphore_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto header_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto adjustment = static_cast<std::int32_t>(context.cpu.gpr[5]);

  std::string error;
  auto object = resolve_dispatcher_object(process, context.memory, header_address, &error);
  if (!object || object->type() != ObjectType::Semaphore) {
    log_resolution_failure("KeReleaseSemaphore",
                           object ? "resolved object is not a Semaphore" : error);
    context.cpu.gpr[3] = 0u;
    return true;
  }

  auto& semaphore = static_cast<kernel::KernelSemaphore&>(*object);
  std::int32_t previous = 0;
  if (!semaphore.release(adjustment, &previous)) {
    // Real KeReleaseSemaphore raises an exception on overflow; Xenon has no
    // guest exception path for this specific case yet, so report the
    // pre-release count unchanged rather than silently claiming success.
    log_resolution_failure("KeReleaseSemaphore", "release would exceed the semaphore's limit");
    context.cpu.gpr[3] = static_cast<std::uint32_t>(semaphore.count());
    return true;
  }
  context.cpu.gpr[3] = static_cast<std::uint32_t>(previous);
  return true;
}

// KeWaitForSingleObject (ordinal 0xB0)
// Guest ABI: r3 = dispatcher object header pointer, r4 = wait reason
// (ignored), r5 = processor mode (ignored), r6 = alertable (ignored),
// r7 = PLARGE_INTEGER timeout (nullable, NULL = infinite) -> r3 = NTSTATUS.
bool ke_wait_for_single_object_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto header_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto timeout_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[7]);

  std::string error;
  auto object = resolve_dispatcher_object(process, context.memory, header_address, &error);
  if (!object) {
    log_resolution_failure("KeWaitForSingleObject", error);
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }

  const auto timeout = read_timeout(context, timeout_ptr);
  const auto result = kernel::wait_for_single_object(object, timeout, context.thread_id);
  context.cpu.gpr[3] = to_status(result, /*signaled_index_as_status=*/0u);
  return true;
}

// KeWaitForMultipleObjects (ordinal 0xAF)
// Guest ABI (matching the real, stable NT KeWaitForMultipleObjects
// signature): r3 = count, r4 = object header pointer array (guest array of
// GUEST ADDRESSES, not handles), r5 = wait type (0 = WaitAny, 1 = WaitAll),
// r6 = wait reason (ignored), r7 = processor mode (ignored), r8 = alertable
// (ignored), r9 = PLARGE_INTEGER timeout (nullable), r10 = wait block array
// (ignored - real-hardware-only kernel bookkeeping, not needed here) ->
// r3 = NTSTATUS.
bool ke_wait_for_multiple_objects_export(kernel::KernelProcess& process,
                                        ExportCallContext& context) {
  const auto count = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  const auto headers_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto wait_all = static_cast<std::uint32_t>(context.cpu.gpr[5]) != 0u;
  const auto timeout_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[9]);

  constexpr std::uint32_t kMaxObjects = 64u;
  if (count == 0u || count > kMaxObjects || headers_ptr == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }

  std::vector<std::shared_ptr<KernelObject>> objects;
  objects.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto header_address = context.memory.read32_be(headers_ptr + i * 4u);
    std::string error;
    auto object = resolve_dispatcher_object(process, context.memory, header_address, &error);
    if (!object) {
      log_resolution_failure("KeWaitForMultipleObjects", error);
      context.cpu.gpr[3] = status::InvalidParameter;
      return true;
    }
    objects.push_back(std::move(object));
  }

  const auto timeout = read_timeout(context, timeout_ptr);
  std::uint32_t signaled_index = 0;
  const auto result =
      kernel::wait_for_multiple_objects(objects, wait_all, timeout, &signaled_index,
                                        context.thread_id);
  context.cpu.gpr[3] = to_status(result, /*signaled_index_as_status=*/signaled_index);
  return true;
}

namespace {

struct KeSyncExportSpec {
  std::uint32_t ordinal;
  const char* name;
};

constexpr KeSyncExportSpec kKeSyncExports[] = {
    {0x070u, "KeInitializeEvent"},
    {0x074u, "KeInitializeSemaphore"},
    {0x08Fu, "KeResetEvent"},
    {0x088u, "KeReleaseSemaphore"},
    {0x09Du, "KeSetEvent"},
    {0x0AFu, "KeWaitForMultipleObjects"},
    {0x0B0u, "KeWaitForSingleObject"},
};

core::ExportHandler handler_for(std::string_view name, kernel::KernelProcess& process) {
  if (name == "KeInitializeEvent") {
    return [&process](ExportCallContext& ctx) { return ke_initialize_event_export(process, ctx); };
  }
  if (name == "KeInitializeSemaphore") {
    return [&process](ExportCallContext& ctx) {
      return ke_initialize_semaphore_export(process, ctx);
    };
  }
  if (name == "KeResetEvent") {
    return [&process](ExportCallContext& ctx) { return ke_reset_event_export(process, ctx); };
  }
  if (name == "KeReleaseSemaphore") {
    return [&process](ExportCallContext& ctx) {
      return ke_release_semaphore_export(process, ctx);
    };
  }
  if (name == "KeSetEvent") {
    return [&process](ExportCallContext& ctx) { return ke_set_event_export(process, ctx); };
  }
  if (name == "KeWaitForMultipleObjects") {
    return [&process](ExportCallContext& ctx) {
      return ke_wait_for_multiple_objects_export(process, ctx);
    };
  }
  if (name == "KeWaitForSingleObject") {
    return [&process](ExportCallContext& ctx) {
      return ke_wait_for_single_object_export(process, ctx);
    };
  }
  return {};
}

}  // namespace

bool register_xboxkrnl_ke_sync_exports(core::ExportRegistry& registry,
                                       kernel::KernelProcess& process) {
  for (const auto& spec : kKeSyncExports) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = spec.name;
    descriptor.ordinal = spec.ordinal;
    descriptor.handler = handler_for(spec.name, process);
    descriptor.requirement = core::ExportRequirement::Required;

    if (!descriptor.handler || !registry.register_export(std::move(descriptor))) {
      return false;
    }
  }

  return true;
}

}  // namespace xenon::xbox
