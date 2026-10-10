#pragma once

// Ke* (kernel-mode) synchronization exports (AC6 Runtime Readiness pass).
// Unlike the Nt* family (xboxkrnl_sync_exports.hpp, handle-based), these
// operate on a raw X_DISPATCH_HEADER-based kernel object embedded directly
// in guest memory, addressed by guest pointer - see
// xex_dispatcher_header.hpp for the verified structure layout and
// resolution mechanism, and docs/kernel/THREADING_V2.md for why Mutant
// (type 2) is intentionally not supported here.
//
// Ordinals verified against the xenia-project/xenia xboxkrnl export table
// (xboxkrnl_table.inc): KeInitializeEvent 0x70, KeInitializeSemaphore 0x74,
// KeResetEvent 0x8F, KeReleaseSemaphore 0x88, KeSetEvent 0x9D,
// KeWaitForMultipleObjects 0xAF, KeWaitForSingleObject 0xB0.

namespace xenon::core {
class ExportRegistry;
struct ExportCallContext;
}
namespace xenon::kernel {
class KernelProcess;
}

namespace xenon::xbox {

[[nodiscard]] bool ke_initialize_event_export(xenon::kernel::KernelProcess& process,
                                              xenon::core::ExportCallContext& context);
[[nodiscard]] bool ke_initialize_semaphore_export(xenon::kernel::KernelProcess& process,
                                                  xenon::core::ExportCallContext& context);
[[nodiscard]] bool ke_set_event_export(xenon::kernel::KernelProcess& process,
                                       xenon::core::ExportCallContext& context);
[[nodiscard]] bool ke_reset_event_export(xenon::kernel::KernelProcess& process,
                                         xenon::core::ExportCallContext& context);
[[nodiscard]] bool ke_release_semaphore_export(xenon::kernel::KernelProcess& process,
                                               xenon::core::ExportCallContext& context);
[[nodiscard]] bool ke_wait_for_single_object_export(xenon::kernel::KernelProcess& process,
                                                    xenon::core::ExportCallContext& context);
[[nodiscard]] bool ke_wait_for_multiple_objects_export(xenon::kernel::KernelProcess& process,
                                                       xenon::core::ExportCallContext& context);
// Not dispatcher-header-based like the rest of this file (see file comment
// above): operates on a guest KTHREAD pointer (XenonSession's per-thread
// GuestKthreadLayout block - see guest_thread_context.hpp), resolved back to
// a KernelThread by scanning kernel::ThreadManager::enumerate_threads() for a
// matching kernel::KernelThread::guest_kthread_address(). Lives here anyway
// since it is still a Ke* export needing KernelProcess at call time.
[[nodiscard]] bool ke_set_base_priority_thread_export(xenon::kernel::KernelProcess& process,
                                                      xenon::core::ExportCallContext& context);
// Same guest-KTHREAD-pointer resolution as ke_set_base_priority_thread_export
// above (see its comment) - the read-only counterpart.
[[nodiscard]] bool ke_query_base_priority_thread_export(xenon::kernel::KernelProcess& process,
                                                        xenon::core::ExportCallContext& context);
// Same guest-KTHREAD-pointer resolution as ke_set_base_priority_thread_export
// above (see its comment).
[[nodiscard]] bool ke_set_affinity_thread_export(xenon::kernel::KernelProcess& process,
                                                 xenon::core::ExportCallContext& context);
// Same guest-KTHREAD-pointer resolution as ke_set_base_priority_thread_export
// above (see its comment).
[[nodiscard]] bool ke_resume_thread_export(xenon::kernel::KernelProcess& process,
                                           xenon::core::ExportCallContext& context);

// Convenience registrar for callers (tests/tools) that already have a
// constructed KernelProcess. XenonSession::init_exports() instead registers
// lambdas that lazily dereference kernel_process_ at call time - see
// xboxkrnl_sync_exports.hpp's identical note for why.
[[nodiscard]] bool register_xboxkrnl_ke_sync_exports(xenon::core::ExportRegistry& registry,
                                                     xenon::kernel::KernelProcess& process);

}  // namespace xenon::xbox
