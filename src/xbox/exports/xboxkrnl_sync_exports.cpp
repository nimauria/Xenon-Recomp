#include "xenon/xbox/xboxkrnl_sync_exports.hpp"

#include "xenon/kernel/completion_port.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/mutant.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/semaphore.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/kernel/time.hpp"
#include "xenon/kernel/timer.hpp"
#include "xenon/kernel/timer_manager.hpp"
#include "xenon/kernel/wait.hpp"
#include "xenon/kernel/xbox_io.hpp"
#include "xenon/logging/logger.hpp"
#include "xenon/logging/probe_log.hpp"
#include "xenon/xbox/xbox_time_convert.hpp"
#include "xbox/exports/sync_events.hpp"
#include "xbox/exports/user_apcs.hpp"

namespace xenon::xbox {
namespace {

using xenon::core::ExportCallContext;
using xenon::logging::events::EventKind;
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
constexpr std::uint32_t kStatusInvalidParameterMix = 0xC0000030u;

// NT WAIT_TYPE: WaitAll is 0 and WaitAny is 1 (`typedef enum _WAIT_TYPE {
// WaitAll, WaitAny, ... }`); xenia selects WaitAny for any nonzero value.
constexpr std::uint32_t kWaitAll = 0u;
constexpr std::uint32_t kWaitAny = 1u;

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
    // STATUS_ABANDONED_WAIT_0 + index, like STATUS_WAIT_0 + index.
    case kernel::WaitResult::Abandoned: return 0x00000080u + signaled_index_as_status;
    // The waiting thread is being terminated; the dispatch loop stops it at
    // its next block boundary.
    case kernel::WaitResult::Terminating: return 0xC000004Bu;  // STATUS_THREAD_IS_TERMINATING
    // An alertable wait ended to run user APCs; they ran before this returns.
    case kernel::WaitResult::UserApc: return 0x000000C0u;  // STATUS_USER_APC
    case kernel::WaitResult::Alerted: return 0x00000101u;  // STATUS_ALERTED
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

bool nt_create_io_completion_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle_out = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  if (handle_out == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  Handle handle{};
  const auto code = process.handle_table().insert(std::make_shared<kernel::IoCompletionPort>(),
                                                  0xFFFFFFFFu, HandleFlags::None, handle);
  if (code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(code);
    return true;
  }
  context.memory.write32_be(handle_out, handle);
  context.cpu.gpr[3] = status::Success;
  return true;
}

namespace {

std::shared_ptr<kernel::IoCompletionPort> lookup_port(kernel::KernelProcess& process, Handle handle) {
  kernel::HandleView view{};
  if (process.handle_table().lookup(handle, view) != KernelIoCode::Success || !view.object ||
      view.object->type() != kernel::ObjectType::IoCompletionPort) {
    return nullptr;
  }
  return std::static_pointer_cast<kernel::IoCompletionPort>(view.object);
}

}  // namespace

bool nt_set_io_completion_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto port = lookup_port(process, static_cast<Handle>(context.cpu.gpr[3]));
  if (!port) {
    context.cpu.gpr[3] = status::InvalidHandle;
    return true;
  }
  kernel::CompletionPacket packet{};
  packet.key = static_cast<std::uint32_t>(context.cpu.gpr[4]);
  packet.context = static_cast<std::uint32_t>(context.cpu.gpr[5]);
  // The guest's NTSTATUS travels in the packet's request id; IoStatus's own
  // code field is the host I/O result, which a guest-posted packet has none of.
  packet.status.request_id = static_cast<std::uint32_t>(context.cpu.gpr[6]);
  packet.status.information = static_cast<std::uint32_t>(context.cpu.gpr[7]);
  port->post(packet);
  context.cpu.gpr[3] = status::Success;
  return true;
}

bool nt_remove_io_completion_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto port = lookup_port(process, static_cast<Handle>(context.cpu.gpr[3]));
  const auto key_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto apc_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[5]);
  const auto iosb_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[6]);
  if (!port) {
    context.cpu.gpr[3] = status::InvalidHandle;
    return true;
  }
  const auto timeout = read_timeout(context, static_cast<cpu::GuestAddress>(context.cpu.gpr[7]));
  const bool infinite = timeout == xbox_infinite_timeout();
  const auto deadline = std::chrono::steady_clock::now() + (infinite ? std::chrono::milliseconds(0) : timeout);
  kernel::CompletionPacket packet{};
  while (!port->try_remove(packet)) {
    auto remaining = infinite ? timeout
                              : std::chrono::duration_cast<std::chrono::milliseconds>(
                                    deadline - std::chrono::steady_clock::now());
    if (!infinite && remaining.count() < 0) remaining = std::chrono::milliseconds(0);
    const auto result = kernel::wait_for_single_object(port->ready_event(), remaining,
                                                       context.thread_id);
    if (result == kernel::WaitResult::Success) continue;  // another remover may win it
    context.cpu.gpr[3] = to_status(result, 0u);
    return true;
  }
  if (key_ptr != 0u) context.memory.write32_be(key_ptr, static_cast<std::uint32_t>(packet.key));
  if (apc_ptr != 0u) context.memory.write32_be(apc_ptr, static_cast<std::uint32_t>(packet.context));
  if (iosb_ptr != 0u) {
    const auto completion_status = packet.status.request_id != 0u
                                       ? static_cast<std::uint32_t>(packet.status.request_id)
                                       : to_status(packet.status.code);
    context.memory.write32_be(iosb_ptr, completion_status);
    context.memory.write32_be(iosb_ptr + 4u, static_cast<std::uint32_t>(packet.status.information));
  }
  context.cpu.gpr[3] = status::Success;
  return true;
}

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
  sync_events::record(EventKind::Signal, context, view.object.get(), handle,
                      static_cast<std::uint32_t>(previous), "NtReleaseSemaphore");
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
  sync_events::record(EventKind::Signal, context, view.object.get(), handle,
                      static_cast<std::uint32_t>(previous), "NtReleaseMutant");
  if (previous_count_ptr != 0u) {
    context.memory.write32_be(previous_count_ptr, static_cast<std::uint32_t>(previous));
  }
  context.cpu.gpr[3] = status::Success;
  return true;
}

// NtClearEvent (ordinal 0xCE)
// Guest ABI: r3 = handle -> r3 = NTSTATUS. The handle-based counterpart to
// KeResetEvent's direct-pointer form (see xboxkrnl_ke_sync_exports.cpp) -
// resets (clears the signaled state of) an event reached through
// kernel::HandleTable rather than a raw guest DISPATCHER_HEADER.
//
// Real AC6 repro: reached during startup with no case registered at all.
bool nt_clear_event_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle = static_cast<Handle>(context.cpu.gpr[3]);

  HandleView view{};
  const auto lookup_code = process.handle_table().lookup(handle, view);
  if (lookup_code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(lookup_code);
    return true;
  }
  if (view.object->type() != ObjectType::Event) {
    context.cpu.gpr[3] = status::ObjectTypeMismatch;
    return true;
  }

  static_cast<kernel::KernelEvent&>(*view.object).reset();
  context.cpu.gpr[3] = status::Success;
  return true;
}

// NtSetEvent (ordinal 0xF6)
// Guest ABI: r3 = handle, r4 = optional guest pointer to receive the
// PREVIOUS signal state (BOOLEAN, nullable) -> r3 = NTSTATUS. The
// handle-based counterpart to KeSetEvent's direct-pointer form.
//
// Real AC6 repro: reached during startup with no case registered at all.
bool nt_set_event_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle = static_cast<Handle>(context.cpu.gpr[3]);
  const auto previous_state_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);

  HandleView view{};
  const auto lookup_code = process.handle_table().lookup(handle, view);
  if (lookup_code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(lookup_code);
    return true;
  }
  if (view.object->type() != ObjectType::Event) {
    context.cpu.gpr[3] = status::ObjectTypeMismatch;
    return true;
  }

  auto& event = static_cast<kernel::KernelEvent&>(*view.object);
  const std::uint32_t previous = event.signaled() ? 1u : 0u;
  event.set();
  sync_events::record(EventKind::Signal, context, view.object.get(), handle, previous,
                      "NtSetEvent");
  if (previous_state_ptr != 0u) {
    context.memory.write32_be(previous_state_ptr, previous);
  }
  context.cpu.gpr[3] = status::Success;
  return true;
}

// NtPulseEvent (ordinal 0xE2)
// Guest ABI: r3 = handle, r4 = optional guest pointer to receive the
// PREVIOUS signal state (BOOLEAN, nullable) -> r3 = NTSTATUS. Same
// signature as NtSetEvent; signals the event and releases current waiters,
// then immediately resets it (verified against the xenia-project/xenia
// reference: XEvent::Pulse()).
//
// A thread that has not yet reached the wait when the pulse happens never
// observes it; every wait already registered that the pulse can satisfy is
// satisfied (KernelEvent::pulse()), as KePulseEvent does on NT.
bool nt_pulse_event_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle = static_cast<Handle>(context.cpu.gpr[3]);
  const auto previous_state_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);

  HandleView view{};
  const auto lookup_code = process.handle_table().lookup(handle, view);
  if (lookup_code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(lookup_code);
    return true;
  }
  if (view.object->type() != ObjectType::Event) {
    context.cpu.gpr[3] = status::ObjectTypeMismatch;
    return true;
  }

  auto& event = static_cast<kernel::KernelEvent&>(*view.object);
  const std::uint32_t previous = event.signaled() ? 1u : 0u;
  event.pulse();
  sync_events::record(EventKind::Signal, context, view.object.get(), handle, previous,
                      "NtPulseEvent");
  if (previous_state_ptr != 0u) {
    context.memory.write32_be(previous_state_ptr, previous);
  }
  context.cpu.gpr[3] = status::Success;
  return true;
}

// NtResumeThread (ordinal 0xF5)
// Guest ABI: r3 = thread handle, r4 = optional guest pointer to receive the
// PREVIOUS suspend count -> r3 = NTSTATUS.
//
// Real AC6 repro: reached during startup with no case registered at all.
bool nt_resume_thread_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle = static_cast<Handle>(context.cpu.gpr[3]);
  const auto previous_count_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);

  HandleView view{};
  const auto lookup_code = process.handle_table().lookup(handle, view);
  if (lookup_code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(lookup_code);
    return true;
  }
  if (view.object->type() != ObjectType::Thread) {
    context.cpu.gpr[3] = status::ObjectTypeMismatch;
    return true;
  }

  auto& thread = static_cast<kernel::KernelThread&>(*view.object);
  const auto previous = thread.suspend_count();
  static_cast<void>(thread.resume());
  if (previous_count_ptr != 0u) {
    context.memory.write32_be(previous_count_ptr, previous);
  }
  context.cpu.gpr[3] = status::Success;
  return true;
}

// ExTerminateThread (ordinal 0x19)
// Guest ABI: r3 = exit code -> does not return (verified against the
// xenia-project/xenia reference: XThread::GetCurrentThread()->Exit(exit_
// code), which the caller never returns from).
//
// dispatch_guest_thread()'s outcome handling (session.cpp) already has a
// dedicated thread_terminated branch that treats thread->exit_code() as
// authoritative over any in-flight register state, and already runs whether
// termination happened via a preemptive safepoint OR (this export's own
// case) the thread calling into a kernel export that terminates itself -
// this only needs to flip that state; the dispatch loop and thread
// trampoline that observe it, unwind cleanly, and report exit_code already
// exist and need no changes. Xenon's dispatch loop observes termination at
// its own safepoints rather than truly never returning to this call's own
// C++ frame (documented latency, not a bug - see KernelThread::terminate()'s
// comment) - guest code after this call may still execute a few more
// instructions before the loop notices, matching how any other external
// termination request already behaves.
bool ex_terminate_thread_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto exit_code = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  auto thread = process.thread_manager().current_thread();
  if (thread) {
    static_cast<void>(thread->terminate(exit_code));
  }
  return true;
}

// NtWaitForSingleObjectEx (ordinal 0xFD)
// Guest ABI: r3 = handle, r4 = wait mode (ignored - Xenon has no distinct
// kernel/user PPC mode), r5 = alertable (user APCs run, STATUS_USER_APC),
// r6 = PLARGE_INTEGER timeout (nullable, NULL = infinite) -> r3 = NTSTATUS
// (STATUS_WAIT_0 / STATUS_TIMEOUT / STATUS_ABANDONED_WAIT_0).
bool nt_wait_for_single_object_ex_export(kernel::KernelProcess& process,
                                         ExportCallContext& context) {
  const auto handle = static_cast<Handle>(context.cpu.gpr[3]);
  const bool alertable = static_cast<std::uint32_t>(context.cpu.gpr[5]) != 0u;
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
  sync_events::record(EventKind::WaitBegin, context, view.object.get(), handle,
                      sync_events::timeout_value(timeout), "NtWaitForSingleObjectEx");
  const auto result =
      kernel::wait_for_single_object(view.object, timeout, context.thread_id, alertable);
  if (result == kernel::WaitResult::UserApc) deliver_user_apcs(context);
  context.cpu.gpr[3] = to_status(result, /*signaled_index_as_status=*/0u);
  sync_events::record(EventKind::WaitEnd, context, view.object.get(), handle,
                      static_cast<std::uint32_t>(context.cpu.gpr[3]), "NtWaitForSingleObjectEx");
  return true;
}

// NtWaitForMultipleObjectsEx (ordinal 0xFE)
// Guest ABI: r3 = count, r4 = PHANDLE array (guest array of dword handles),
// r5 = wait type (NT WAIT_TYPE: 0 = WaitAll, 1 = WaitAny), r6 = wait mode
// (ignored), r7 = alertable (user APCs run, STATUS_USER_APC), r8 = PLARGE_INTEGER timeout
// (nullable) -> r3 = NTSTATUS (STATUS_WAIT_0 + signaled index for WaitAny,
// STATUS_SUCCESS for WaitAll). As in NT's object-manager wait, a WaitAll
// naming the same object twice (through any handles) fails with
// STATUS_INVALID_PARAMETER_MIX: it could never be satisfied.
bool nt_wait_for_multiple_objects_ex_export(kernel::KernelProcess& process,
                                            ExportCallContext& context) {
  const auto count = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  const auto handles_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto wait_type = static_cast<std::uint32_t>(context.cpu.gpr[5]);
  const bool wait_all = wait_type == kWaitAll;
  const bool alertable = static_cast<std::uint32_t>(context.cpu.gpr[7]) != 0u;
  const auto timeout_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[8]);

  constexpr std::uint32_t kMaxObjects = 64u;
  if (count == 0u || count > kMaxObjects || handles_ptr == 0u ||
      (wait_type != kWaitAll && wait_type != kWaitAny)) {
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
    if (wait_all && std::find(objects.begin(), objects.end(), view.object) != objects.end()) {
      context.cpu.gpr[3] = kStatusInvalidParameterMix;
      return true;
    }
    objects.push_back(view.object);
  }

  const auto timeout = read_timeout(context, timeout_ptr);
  for (std::uint32_t i = 0; i < count; ++i) {
    sync_events::record(EventKind::WaitBegin, context, objects[i].get(),
                        context.memory.read32_be(handles_ptr + i * 4u),
                        sync_events::timeout_value(timeout), "NtWaitForMultipleObjectsEx");
  }
  std::uint32_t signaled_index = 0;
  const auto result = kernel::wait_for_multiple_objects(
      objects, wait_all, timeout, &signaled_index, context.thread_id, alertable);
  if (result == kernel::WaitResult::UserApc) deliver_user_apcs(context);
  context.cpu.gpr[3] = to_status(result, /*signaled_index_as_status=*/signaled_index);
  // A WaitAny names the object that satisfied it; a WaitAll or a failed wait
  // has no single object.
  const bool took_one =
      result == kernel::WaitResult::Success || result == kernel::WaitResult::Abandoned;
  const auto* satisfied = !wait_all && took_one && signaled_index < objects.size()
                              ? objects[signaled_index].get()
                              : nullptr;
  sync_events::record(EventKind::WaitEnd, context, satisfied, 0u,
                      static_cast<std::uint32_t>(context.cpu.gpr[3]), "NtWaitForMultipleObjectsEx");
  return true;
}

// NtCreateTimer (ordinal 0xD7)
// Guest ABI: r3 = PHANDLE out, r4 = POBJECT_ATTRIBUTES (unnamed-only, see
// NtCreateEvent), r5 = timer type (0 = NotificationTimer/manual-reset, 1 =
// SynchronizationTimer/auto-reset - same convention as EVENT_TYPE) ->
// r3 = NTSTATUS.
bool nt_create_timer_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle_out = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto timer_type = static_cast<std::uint32_t>(context.cpu.gpr[5]);
  if (handle_out == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }

  const auto type = (timer_type == 0u) ? kernel::TimerType::NotificationTimer
                                       : kernel::TimerType::SynchronizationTimer;
  auto timer = std::make_shared<kernel::KernelTimer>(type);

  Handle handle{};
  const auto code =
      process.handle_table().insert(std::move(timer), /*granted_access=*/0xFFFFFFFFu,
                                    HandleFlags::None, handle);
  if (code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(code);
    return true;
  }

  context.memory.write32_be(handle_out, handle);
  context.cpu.gpr[3] = status::Success;
  return true;
}

// NtCancelTimer (ordinal 0xCD)
// Guest ABI: r3 = handle, r4 = PLONG current-state out (optional, nullable;
// real xboxkrnl always writes 0 here per the reference implementation, not
// a meaningful previous-state value) -> r3 = NTSTATUS.
bool nt_cancel_timer_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle = static_cast<Handle>(context.cpu.gpr[3]);
  const auto current_state_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);

  HandleView view{};
  const auto lookup_code = process.handle_table().lookup(handle, view);
  if (lookup_code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(lookup_code);
    return true;
  }
  if (view.object->type() != ObjectType::Timer) {
    context.cpu.gpr[3] = status::ObjectTypeMismatch;
    return true;
  }

  auto timer = std::static_pointer_cast<kernel::KernelTimer>(view.object);
  static_cast<void>(timer->cancel());
  process.timer_manager().unschedule(timer);

  if (current_state_ptr != 0u) {
    context.memory.write32_be(current_state_ptr, 0u);
  }
  context.cpu.gpr[3] = status::Success;
  return true;
}

// NtSetTimerEx (ordinal 0xFA)
// Guest ABI: r3 = handle, r4 = PLARGE_INTEGER due time (100ns units, same
// convention as wait timeouts - negative = relative, positive = absolute,
// mandatory), r5 = timer APC routine (nullable), r6 = unk (ignored), r7 =
// routine argument, r8 = resume (ignored - no suspend semantics modeled),
// r9 = period in milliseconds (0 = one-shot), r10 = unk (ignored) ->
// r3 = NTSTATUS. Each firing queues Routine(Argument, TimeLow, TimeHigh) as
// a user APC to the thread that set the timer (the time is the system time
// of the firing), as xenia and rexglue-sdk do; it runs at that thread's
// next alertable wait.
bool nt_set_timer_ex_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle = static_cast<Handle>(context.cpu.gpr[3]);
  const auto due_time_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto routine_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[5]);
  const auto routine_arg = static_cast<std::uint32_t>(context.cpu.gpr[7]);
  const auto period_ms = static_cast<std::uint32_t>(context.cpu.gpr[9]);

  if (due_time_ptr == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }

  HandleView view{};
  const auto lookup_code = process.handle_table().lookup(handle, view);
  if (lookup_code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(lookup_code);
    return true;
  }
  if (view.object->type() != ObjectType::Timer) {
    context.cpu.gpr[3] = status::ObjectTypeMismatch;
    return true;
  }

  kernel::TimerCallback queue_timer_apc;
  if (routine_ptr != 0u) {
    // The setting thread may exit before the timer fires; its APCs then go
    // nowhere, as on hardware.
    std::weak_ptr<kernel::KernelThread> target =
        process.thread_manager().get_thread(context.thread_id);
    queue_timer_apc = [target, routine_ptr, routine_arg] {
      if (auto thread = target.lock()) {
        const auto now = kernel::TimeServices::system_time();
        thread->queue_user_apc({routine_ptr, routine_arg, static_cast<std::uint32_t>(now),
                                static_cast<std::uint32_t>(now >> 32)});
      }
    };
  }
  xenon::logging::append_probe_log("nt_set_timer_ex_diag.log",
                                   "NtSetTimerEx: handle=0x%08X routine_ptr=0x%08X period_ms=%u\n",
                                   (unsigned)handle, (unsigned)routine_ptr, (unsigned)period_ms);

  const auto raw = static_cast<std::int64_t>(context.memory.read64_be(due_time_ptr));
  const auto relative_due = xbox_timeout_to_relative_ms(raw);
  const std::optional<std::chrono::milliseconds> period =
      period_ms == 0u ? std::nullopt
                      : std::optional<std::chrono::milliseconds>(period_ms);

  auto timer = std::static_pointer_cast<kernel::KernelTimer>(view.object);
  if (!timer->set(relative_due, period, std::move(queue_timer_apc))) {
    context.cpu.gpr[3] = status::Unsuccessful;
    return true;
  }
  if (relative_due.count() > 0) {
    // due_time == 0 already fired synchronously inside set(); only a
    // genuinely future due time needs the dispatch thread.
    process.timer_manager().schedule(timer);
  }

  context.cpu.gpr[3] = status::Success;
  return true;
}

// NtSignalAndWaitForSingleObjectEx (ordinal 0xFB / 251)
// Guest ABI (verified against rexglue-sdk's
// NtSignalAndWaitForSingleObjectEx_entry register mapping): r3 = signal
// handle, r4 = wait handle, r5 = alertable (user APCs run,
// STATUS_USER_APC), r6 = unused by that same reference
// implementation, r7 = PLARGE_INTEGER timeout (nullable) -> r3 = NTSTATUS.
// Signals signal_handle (Event -> Set, Semaphore -> Release(1), Mutant ->
// Release) then waits on wait_handle. As on NT, the signal and the start of
// the wait are one dispatcher operation (kernel::signal_and_wait_for_object):
// a thread woken by the signal, or one that takes a mutant it releases,
// cannot signal the wait object before this thread is waiting on it. Titles
// build condition variables from exactly this (AC6: release the lock mutant,
// wait on an auto-reset event); done as two steps, the other thread's
// SetEvent could land before this wait and be consumed by its own next wait.
// Both handles are resolved before anything is signalled.
bool nt_signal_and_wait_for_single_object_ex_export(kernel::KernelProcess& process,
                                                     ExportCallContext& context) {
  const auto signal_handle = static_cast<Handle>(context.cpu.gpr[3]);
  const auto wait_handle = static_cast<Handle>(context.cpu.gpr[4]);
  const bool alertable = static_cast<std::uint32_t>(context.cpu.gpr[5]) != 0u;
  const auto timeout_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[7]);

  HandleView signal_view{};
  const auto signal_lookup = process.handle_table().lookup(signal_handle, signal_view);
  if (signal_lookup != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(signal_lookup);
    return true;
  }
  const auto signal_type = signal_view.object->type();
  if (signal_type != ObjectType::Event && signal_type != ObjectType::Semaphore &&
      signal_type != ObjectType::Mutant) {
    context.cpu.gpr[3] = status::ObjectTypeMismatch;
    return true;
  }
  HandleView wait_view{};
  const auto wait_lookup = process.handle_table().lookup(wait_handle, wait_view);
  if (wait_lookup != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(wait_lookup);
    return true;
  }
  if (!kernel::is_waitable_object(wait_view.object->type())) {
    context.cpu.gpr[3] = status::ObjectTypeMismatch;
    return true;
  }

  const auto timeout = read_timeout(context, timeout_ptr);
  std::uint32_t signal_failure = 0;
  // Runs with the dispatcher lock held.
  const auto signal = [&]() -> bool {
    auto& object = *signal_view.object;
    if (signal_type == ObjectType::Event) {
      static_cast<kernel::KernelEvent&>(object).set_locked();
    } else if (signal_type == ObjectType::Semaphore) {
      if (!static_cast<kernel::KernelSemaphore&>(object).release_locked(1)) {
        signal_failure = kStatusSemaphoreLimitExceeded;
        return false;
      }
    } else if (!static_cast<kernel::KernelMutant&>(object).release_locked(context.thread_id)) {
      signal_failure = kStatusMutantNotOwned;
      return false;
    }
    sync_events::record(EventKind::Signal, context, &object, signal_handle, 0u,
                        "NtSignalAndWaitForSingleObjectEx");
    sync_events::record(EventKind::WaitBegin, context, wait_view.object.get(), wait_handle,
                        sync_events::timeout_value(timeout), "NtSignalAndWaitForSingleObjectEx");
    return true;
  };
  const auto result = kernel::signal_and_wait_for_object(signal, wait_view.object, timeout,
                                                         context.thread_id, alertable);
  if (signal_failure != 0u) {
    context.cpu.gpr[3] = signal_failure;
    return true;
  }
  if (result == kernel::WaitResult::UserApc) deliver_user_apcs(context);
  context.cpu.gpr[3] = to_status(result, /*signaled_index_as_status=*/0u);
  sync_events::record(EventKind::WaitEnd, context, wait_view.object.get(), wait_handle,
                      static_cast<std::uint32_t>(context.cpu.gpr[3]),
                      "NtSignalAndWaitForSingleObjectEx");
  return true;
}

// NtClose (ordinal 0xCF)
// Guest ABI: r3 = handle -> r3 = NTSTATUS. Closes any kernel object handle
// generically (event, semaphore, mutant, timer, file, etc.) - real hardware
// does not require the caller to know the handle's object type, unlike the
// type-specific Nt*Release* exports above.
bool nt_close_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle = static_cast<Handle>(context.cpu.gpr[3]);
  const auto code = process.handle_table().close(handle);
  context.cpu.gpr[3] = to_status(code);
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
    {0x0D7u, "NtCreateTimer"},
    {0x0CDu, "NtCancelTimer"},
    {0x0FAu, "NtSetTimerEx"},
    {0x0CEu, "NtClearEvent"},
    {0x0F6u, "NtSetEvent"},
    {0x0E2u, "NtPulseEvent"},
    {0x019u, "ExTerminateThread"},
    {0x0F5u, "NtResumeThread"},
    {0x0CFu, "NtClose"},
    {0x0FBu, "NtSignalAndWaitForSingleObjectEx"},
    {0x0D3u, "NtCreateIoCompletion"},
    {0x0F8u, "NtSetIoCompletion"},
    {0x0F4u, "NtRemoveIoCompletion"},
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
  if (name == "NtCreateTimer") {
    return [&process](ExportCallContext& ctx) { return nt_create_timer_export(process, ctx); };
  }
  if (name == "NtCancelTimer") {
    return [&process](ExportCallContext& ctx) { return nt_cancel_timer_export(process, ctx); };
  }
  if (name == "NtSetTimerEx") {
    return [&process](ExportCallContext& ctx) { return nt_set_timer_ex_export(process, ctx); };
  }
  if (name == "NtClearEvent") {
    return [&process](ExportCallContext& ctx) { return nt_clear_event_export(process, ctx); };
  }
  if (name == "NtSetEvent") {
    return [&process](ExportCallContext& ctx) { return nt_set_event_export(process, ctx); };
  }
  if (name == "NtPulseEvent") {
    return [&process](ExportCallContext& ctx) { return nt_pulse_event_export(process, ctx); };
  }
  if (name == "ExTerminateThread") {
    return [&process](ExportCallContext& ctx) { return ex_terminate_thread_export(process, ctx); };
  }
  if (name == "NtResumeThread") {
    return [&process](ExportCallContext& ctx) { return nt_resume_thread_export(process, ctx); };
  }
  if (name == "NtClose") {
    return [&process](ExportCallContext& ctx) { return nt_close_export(process, ctx); };
  }
  if (name == "NtCreateIoCompletion") {
    return [&process](ExportCallContext& ctx) { return nt_create_io_completion_export(process, ctx); };
  }
  if (name == "NtSetIoCompletion") {
    return [&process](ExportCallContext& ctx) { return nt_set_io_completion_export(process, ctx); };
  }
  if (name == "NtRemoveIoCompletion") {
    return [&process](ExportCallContext& ctx) { return nt_remove_io_completion_export(process, ctx); };
  }
  if (name == "NtSignalAndWaitForSingleObjectEx") {
    return [&process](ExportCallContext& ctx) {
      return nt_signal_and_wait_for_single_object_ex_export(process, ctx);
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
