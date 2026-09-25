#pragma once

namespace xenon::core {
class ExportRegistry;
struct ExportCallContext;
}
namespace xenon::kernel {
class KernelProcess;
}

namespace xenon::xbox {

// Per-export handlers, exposed individually so a caller whose
// kernel::KernelProcess is not yet constructed at export-registration time
// (XenonSession::init_exports() runs before any title is loaded) can
// register its own lambda that dereferences its own KernelProcess pointer at
// CALL time and forwards here, rather than needing register_xboxkrnl_sync_exports()
// below (which requires a already-constructed process reference up front -
// fine for tests/tools that construct one directly).
[[nodiscard]] bool nt_create_event_export(xenon::kernel::KernelProcess& process,
                                          xenon::core::ExportCallContext& context);
[[nodiscard]] bool nt_create_semaphore_export(xenon::kernel::KernelProcess& process,
                                              xenon::core::ExportCallContext& context);
[[nodiscard]] bool nt_release_semaphore_export(xenon::kernel::KernelProcess& process,
                                               xenon::core::ExportCallContext& context);
[[nodiscard]] bool nt_create_mutant_export(xenon::kernel::KernelProcess& process,
                                           xenon::core::ExportCallContext& context);
[[nodiscard]] bool nt_release_mutant_export(xenon::kernel::KernelProcess& process,
                                            xenon::core::ExportCallContext& context);
[[nodiscard]] bool nt_wait_for_single_object_ex_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool nt_wait_for_multiple_objects_ex_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool nt_create_timer_export(xenon::kernel::KernelProcess& process,
                                          xenon::core::ExportCallContext& context);
[[nodiscard]] bool nt_cancel_timer_export(xenon::kernel::KernelProcess& process,
                                          xenon::core::ExportCallContext& context);
[[nodiscard]] bool nt_set_timer_ex_export(xenon::kernel::KernelProcess& process,
                                          xenon::core::ExportCallContext& context);

// Registers the xboxkrnl handle-based (Nt*) synchronization exports this
// pass adds: NtCreateEvent, NtCreateSemaphore, NtReleaseSemaphore,
// NtCreateMutant, NtReleaseMutant, NtWaitForSingleObjectEx,
// NtWaitForMultipleObjectsEx, NtCreateTimer, NtCancelTimer, NtSetTimerEx.
// Ordinals were verified against the xenia-project/xenia xboxkrnl export
// table (xboxkrnl_table.inc), not guessed - see docs/kernel/THREADING_V2.md.
//
// NtSetTimerEx's guest callback ROUTINE parameter is accepted but not
// invoked: real Xbox 360 timer callbacks run in a DPC/APC context Xenon does
// not model, and inventing one without a verified reference risks silently
// wrong guest-visible behavior. The timer object itself still fires and
// signals correctly for NtWaitForSingleObjectEx-style waiters; a nonzero
// routine pointer is logged as a diagnostic, not silently dropped. See
// docs/kernel/THREADING_V2.md.
//
// Deliberately does NOT register the Ke* variants (KeSetEvent, KeResetEvent,
// KeWaitForSingleObject, KeWaitForMultipleObjects): those operate on a raw
// DISPATCHER_HEADER-based kernel object embedded directly in guest memory
// (part of the game's own data structures), addressed by guest pointer
// rather than a Handle. Xenon's KernelEvent/KernelSemaphore/KernelMutant are
// host-side objects reached only through kernel::HandleTable, so supporting
// the Ke* variants correctly requires modeling that guest-memory layout and
// correlating it with a host object - a distinct, independently-verifiable
// piece of work, intentionally not attempted here rather than guessed at.
// See docs/kernel/THREADING_V2.md for the tracked follow-up.
//
// Every object this registers creates is published through
// process.handle_table(), the shared per-process dispatcher-object handle
// table (kernel::KernelProcess::handle_table()) - the same table
// ExCreateThread (once implemented) will use for thread handles, matching
// real Xbox 360 semantics where every kernel object shares one per-process
// handle namespace.
//
// process must outlive every export call made through registry - callers
// that register this before process exists (e.g. XenonSession::init_exports,
// which runs before a title is loaded) must instead register lambdas that
// dereference their own lazily-bound KernelProcess pointer at call time and
// forward to the *_export free functions below, rather than calling this
// convenience registrar directly. See XenonSession::init_thread_sync_exports().
[[nodiscard]] bool register_xboxkrnl_sync_exports(
    xenon::core::ExportRegistry& registry, xenon::kernel::KernelProcess& process);

}  // namespace xenon::xbox
