#include "xenon/xbox/xboxkrnl_sync_exports.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/mutant.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/semaphore.hpp"
#include "xenon/kernel/wait.hpp"
#include "xenon/kernel/xbox_io.hpp"
#include "xenon/xbox/xbox_time_convert.hpp"

namespace xenon::xbox {
namespace {

using xenon::core::ExportCallContext;
using xenon::kernel::Handle;
using xenon::kernel::HandleFlags;
using xenon::kernel::HandleView;
using xenon::kernel::KernelIoCode;
using xenon::kernel::KernelObject;
using xenon::kernel::ObjectType;

namespace status = xenon::kernel::xbox::status;

// STATUS_MUTANT_NOT_OWNED and STATUS_SEMAPHORE_LIMIT_EXCEEDED are stable,
// widely-published NT status codes (not xboxkrnl-specific ordinals, so no
// external verification is needed the way export ordinals require) that
// xenon::kernel::xbox::status does not yet define.
constexpr std::uint32_t kStatusMutantNotOwned = 0xC0000046u;
constexpr std::uint32_t kStatusSemaphoreLimitExceeded = 0xC0000047u;

std::uint32_t to_status(KernelIoCode code) {
  switch (code) {
    case KernelIoCode::Success: return status::Success;
    case KernelIoCode::InvalidHandle: return status::InvalidHandle;
    case KernelIoCode::InvalidObjectType: return status::ObjectTypeMismatch;
    case KernelIoCode::InvalidParameter: return status::InvalidParameter;
    case KernelIoCode::AccessDenied: return status::AccessDenied;
    case KernelIoCode::ProtectedHandle: return status::AccessDenied;
    default: return status::Unsuccessful;
  }
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

// Reads a nullable Xbox LARGE_INTEGER timeout pointer (NULL = wait forever).
std::chrono::milliseconds read_timeout(ExportCallContext& context,
                                       cpu::GuestAddress timeout_ptr) {
  if (timeout_ptr == 0u) {
    return xbox_infinite_timeout();
  }
  const auto raw = static_cast<std::int64_t>(context.memory.read64_be(timeout_ptr));
  return xbox_timeout_to_relative_ms(raw);
}

}  // namespace

// NtCreateEvent (ordinal 0xD1)
// Guest ABI: r3 = PHANDLE out, r4 = POBJECT_ATTRIBUTES (unnamed-only support -
// a non-null value with a name is not looked up/deduplicated by this pass),
// r5 = EVENT_TYPE (0 = NotificationEvent/manual-reset, 1 =
// SynchronizationEvent/auto-reset - the real, stable NT EVENT_TYPE enum),
// r6 = initial state (BOOLEAN) -> r3 = NTSTATUS.
bool nt_create_event_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle_out = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto event_type = static_cast<std::uint32_t>(context.cpu.gpr[5]);
  const auto initial_state = static_cast<std::uint32_t>(context.cpu.gpr[6]) != 0u;
  if (handle_out == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }

  const bool manual_reset = (event_type == 0u);
  auto event = std::make_shared<kernel::KernelEvent>(manual_reset, initial_state);

  Handle handle{};
  const auto code =
      process.handle_table().insert(std::move(event), /*granted_access=*/0xFFFFFFFFu,
                                    HandleFlags::None, handle);
  if (code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(code);
    return true;
  }

  context.memory.write32_be(handle_out, handle);
  context.cpu.gpr[3] = status::Success;
  return true;
}

// NtCreateSemaphore (ordinal 0xD5)
// Guest ABI: r3 = PHANDLE out, r4 = POBJECT_ATTRIBUTES (unnamed-only, see
// NtCreateEvent), r5 = initial count (LONG), r6 = maximum count (LONG) ->
// r3 = NTSTATUS.
bool nt_create_semaphore_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle_out = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto initial_count = static_cast<std::int32_t>(context.cpu.gpr[5]);
  const auto maximum_count = static_cast<std::int32_t>(context.cpu.gpr[6]);
  if (handle_out == 0u || maximum_count <= 0 || initial_count < 0 ||
      initial_count > maximum_count) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }

  auto semaphore = std::make_shared<kernel::KernelSemaphore>(initial_count, maximum_count);
  Handle handle{};
  const auto code =
      process.handle_table().insert(std::move(semaphore), /*granted_access=*/0xFFFFFFFFu,
                                    HandleFlags::None, handle);
  if (code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(code);
    return true;
  }

  context.memory.write32_be(handle_out, handle);
  context.cpu.gpr[3] = status::Success;
  return true;
}

// NtReleaseSemaphore (ordinal 0xF3)
// Guest ABI: r3 = handle, r4 = release count (LONG), r5 = PLONG previous
// count out (optional, nullable) -> r3 = NTSTATUS.
bool nt_release_semaphore_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle = static_cast<Handle>(context.cpu.gpr[3]);
  const auto release_count = static_cast<std::int32_t>(context.cpu.gpr[4]);
  const auto previous_count_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[5]);

  HandleView view{};
  const auto lookup_code = process.handle_table().lookup(handle, view);
  if (lookup_code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(lookup_code);
    return true;
  }
  if (view.object->type() != ObjectType::Semaphore) {
    context.cpu.gpr[3] = status::ObjectTypeMismatch;
    return true;
  }

  auto& semaphore = static_cast<kernel::KernelSemaphore&>(*view.object);
  std::int32_t previous = 0;
  if (!semaphore.release(release_count, &previous)) {
    context.cpu.gpr[3] = kStatusSemaphoreLimitExceeded;
    return true;
  }
  if (previous_count_ptr != 0u) {
    context.memory.write32_be(previous_count_ptr, static_cast<std::uint32_t>(previous));
  }
  context.cpu.gpr[3] = status::Success;
  return true;
}

// NtCreateMutant (ordinal 0xD4)
// Guest ABI: r3 = PHANDLE out, r4 = POBJECT_ATTRIBUTES (unnamed-only, see
// NtCreateEvent), r5 = initial owner (BOOLEAN) -> r3 = NTSTATUS.
//
// The creating thread's real id (context.thread_id) becomes the mutant's
// initial owner when requested - fixing the historical hardcoded-owner-id-1
// placeholder (see kernel::KernelMutant's constructor comment) by construction,
// since this is the only production call site that constructs an
// initially-owned KernelMutant.
bool nt_create_mutant_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle_out = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto initial_owner = static_cast<std::uint32_t>(context.cpu.gpr[5]) != 0u;
  if (handle_out == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }

  auto mutant = std::make_shared<kernel::KernelMutant>(initial_owner, context.thread_id);
  Handle handle{};
  const auto code =
      process.handle_table().insert(std::move(mutant), /*granted_access=*/0xFFFFFFFFu,
                                    HandleFlags::None, handle);
  if (code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(code);
    return true;
  }

  context.memory.write32_be(handle_out, handle);
  context.cpu.gpr[3] = status::Success;
  return true;
}

// NtReleaseMutant (ordinal 0xF2)
// Guest ABI: r3 = handle, r4 = PLONG previous recursion count out (optional,
// nullable, matching NtReleaseSemaphore's sibling parameter) -> r3 = NTSTATUS.
bool nt_release_mutant_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle = static_cast<Handle>(context.cpu.gpr[3]);
  const auto previous_count_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);

  HandleView view{};
  const auto lookup_code = process.handle_table().lookup(handle, view);
  if (lookup_code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(lookup_code);
    return true;
  }
  if (view.object->type() != ObjectType::Mutant) {
    context.cpu.gpr[3] = status::ObjectTypeMismatch;
    return true;
  }

  auto& mutant = static_cast<kernel::KernelMutant&>(*view.object);
  const auto previous = mutant.recursion_count();
  if (!mutant.release(context.thread_id)) {
    context.cpu.gpr[3] = kStatusMutantNotOwned;
    return true;
  }
  if (previous_count_ptr != 0u) {
    context.memory.write32_be(previous_count_ptr, static_cast<std::uint32_t>(previous));
  }
  context.cpu.gpr[3] = status::Success;
  return true;
}

// NtWaitForSingleObjectEx (ordinal 0xFD)
// Guest ABI: r3 = handle, r4 = wait mode (ignored - Xenon has no distinct
// kernel/user PPC mode), r5 = alertable (ignored - no APC delivery yet),
// r6 = PLARGE_INTEGER timeout (nullable, NULL = infinite) -> r3 = NTSTATUS
// (STATUS_WAIT_0 / STATUS_TIMEOUT / STATUS_ABANDONED_WAIT_0).
bool nt_wait_for_single_object_ex_export(kernel::KernelProcess& process,
                                         ExportCallContext& context) {
  const auto handle = static_cast<Handle>(context.cpu.gpr[3]);
  const auto timeout_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[6]);

  HandleView view{};
  const auto lookup_code = process.handle_table().lookup(handle, view);
  if (lookup_code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(lookup_code);
    return true;
  }
  if (!kernel::is_waitable_object(view.object->type())) {
    context.cpu.gpr[3] = status::ObjectTypeMismatch;
    return true;
  }

  const auto timeout = read_timeout(context, timeout_ptr);
  const auto result =
      kernel::wait_for_single_object(view.object, timeout, context.thread_id);
  context.cpu.gpr[3] = to_status(result, /*signaled_index_as_status=*/0u);
  return true;
}

// NtWaitForMultipleObjectsEx (ordinal 0xFE)
// Guest ABI: r3 = count, r4 = PHANDLE array (guest array of dword handles),
// r5 = wait type (0 = any/WaitAny, 1 = all/WaitAll), r6 = wait mode
// (ignored), r7 = alertable (ignored), r8 = PLARGE_INTEGER timeout
// (nullable) -> r3 = NTSTATUS (STATUS_WAIT_0 + signaled index for WaitAny,
// STATUS_SUCCESS for WaitAll).
bool nt_wait_for_multiple_objects_ex_export(kernel::KernelProcess& process,
                                            ExportCallContext& context) {
  const auto count = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  const auto handles_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto wait_all = static_cast<std::uint32_t>(context.cpu.gpr[5]) != 0u;
  const auto timeout_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[8]);

  constexpr std::uint32_t kMaxObjects = 64u;
  if (count == 0u || count > kMaxObjects || handles_ptr == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }

  std::vector<std::shared_ptr<KernelObject>> objects;
  objects.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto handle =
        static_cast<Handle>(context.memory.read32_be(handles_ptr + i * 4u));
    HandleView view{};
    const auto lookup_code = process.handle_table().lookup(handle, view);
    if (lookup_code != KernelIoCode::Success) {
      context.cpu.gpr[3] = to_status(lookup_code);
      return true;
    }
    if (!kernel::is_waitable_object(view.object->type())) {
      context.cpu.gpr[3] = status::ObjectTypeMismatch;
      return true;
    }
    objects.push_back(view.object);
  }

  const auto timeout = read_timeout(context, timeout_ptr);
  std::uint32_t signaled_index = 0;
  const auto result = kernel::wait_for_multiple_objects(
      objects, wait_all, timeout, &signaled_index, context.thread_id);
  context.cpu.gpr[3] = to_status(result, /*signaled_index_as_status=*/signaled_index);
  return true;
}

namespace {

struct SyncExportSpec {
  std::uint32_t ordinal;
  const char* name;
};

// Ordinals verified against the xenia-project/xenia xboxkrnl export table
// (xboxkrnl_table.inc), not guessed - see xboxkrnl_sync_exports.hpp.
constexpr SyncExportSpec kSyncExports[] = {
    {0x0D1u, "NtCreateEvent"},
    {0x0D5u, "NtCreateSemaphore"},
    {0x0F3u, "NtReleaseSemaphore"},
    {0x0D4u, "NtCreateMutant"},
    {0x0F2u, "NtReleaseMutant"},
    {0x0FDu, "NtWaitForSingleObjectEx"},
    {0x0FEu, "NtWaitForMultipleObjectsEx"},
};

core::ExportHandler handler_for(std::string_view name, kernel::KernelProcess& process) {
  if (name == "NtCreateEvent") {
    return [&process](ExportCallContext& ctx) { return nt_create_event_export(process, ctx); };
  }
  if (name == "NtCreateSemaphore") {
    return [&process](ExportCallContext& ctx) {
      return nt_create_semaphore_export(process, ctx);
    };
  }
  if (name == "NtReleaseSemaphore") {
    return [&process](ExportCallContext& ctx) {
      return nt_release_semaphore_export(process, ctx);
    };
  }
  if (name == "NtCreateMutant") {
    return [&process](ExportCallContext& ctx) { return nt_create_mutant_export(process, ctx); };
  }
  if (name == "NtReleaseMutant") {
    return [&process](ExportCallContext& ctx) {
      return nt_release_mutant_export(process, ctx);
    };
  }
  if (name == "NtWaitForSingleObjectEx") {
    return [&process](ExportCallContext& ctx) {
      return nt_wait_for_single_object_ex_export(process, ctx);
    };
  }
  if (name == "NtWaitForMultipleObjectsEx") {
    return [&process](ExportCallContext& ctx) {
      return nt_wait_for_multiple_objects_ex_export(process, ctx);
    };
  }
  return {};
}

}  // namespace

bool register_xboxkrnl_sync_exports(core::ExportRegistry& registry,
                                    kernel::KernelProcess& process) {
  for (const auto& spec : kSyncExports) {
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
