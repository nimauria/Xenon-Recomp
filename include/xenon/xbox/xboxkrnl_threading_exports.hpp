#pragma once

namespace xenon::core {
class ExportRegistry;
struct ExportCallContext;
}
namespace xenon::kernel {
class KernelProcess;
}

namespace xenon::xbox {

// Kernel threading/synchronization exports beyond the core Ke*/Nt* set. Ordinals
// are from the xenia xboxkrnl export table.

// --- Process-bound (bound lazily in XenonSession::init_exports()) ------------

// Executive read/write lock (ERWLOCK, 0x38 bytes: lock count, writers-waiting,
// readers-waiting, readers-entry, a synchronization KEVENT and a KSEMAPHORE).
// The standard NT algorithm: a negative lock count means free; writers wait on
// the event, readers on the semaphore.
[[nodiscard]] bool ex_initialize_read_write_lock_export(xenon::kernel::KernelProcess& process,
                                                        xenon::core::ExportCallContext& context);
[[nodiscard]] bool ex_acquire_read_write_lock_exclusive_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool ex_acquire_read_write_lock_shared_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool ex_try_to_acquire_read_write_lock_exclusive_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool ex_try_to_acquire_read_write_lock_shared_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool ex_release_read_write_lock_export(xenon::kernel::KernelProcess& process,
                                                     xenon::core::ExportCallContext& context);

// RtlRaiseException (0x136): r3 = EXCEPTION_RECORD* -> returns only if the
// exception is continued. Xenon has no guest SEH unwinder: the MSVC
// thread-naming exception (0x406D1388) names the thread and returns, as the
// __except handler around it would; any other exception is logged and
// recorded as ExceptionNotDispatched, and the call returns.
[[nodiscard]] bool rtl_raise_exception_export(xenon::kernel::KernelProcess& process,
                                              xenon::core::ExportCallContext& context);

// NtQueueApcThread (0xE3): r3 = thread handle (0xFFFFFFFE = the calling
// thread), r4 = APC routine, r5 = routine context, r6 = argument 1, r7 =
// argument 2 -> NTSTATUS. Queues Routine(Context, Argument1, Argument2) as a
// user APC; it runs at the target thread's next alertable wait or delay.
[[nodiscard]] bool nt_queue_apc_thread_export(xenon::kernel::KernelProcess& process,
                                              xenon::core::ExportCallContext& context);

// KePulseEvent (0x7F): r3 = KEVENT*, r4 = increment, r5 = wait -> r3 = previous state.
[[nodiscard]] bool ke_pulse_event_export(xenon::kernel::KernelProcess& process,
                                         xenon::core::ExportCallContext& context);
// NtQueryEvent (0xE6): r3 = event handle, r4 = EVENT_BASIC_INFORMATION* -> NTSTATUS.
[[nodiscard]] bool nt_query_event_export(xenon::kernel::KernelProcess& process,
                                         xenon::core::ExportCallContext& context);
// KeSuspendThread (0xA9): r3 = KTHREAD* -> r3 = previous suspend count.
[[nodiscard]] bool ke_suspend_thread_export(xenon::kernel::KernelProcess& process,
                                            xenon::core::ExportCallContext& context);
// NtSuspendThread (0xFC): r3 = thread handle, r4 = PULONG previous count -> NTSTATUS.
[[nodiscard]] bool nt_suspend_thread_export(xenon::kernel::KernelProcess& process,
                                            xenon::core::ExportCallContext& context);
// KeSetDisableBoostThread (0x9C): r3 = KTHREAD*, r4 = disabled -> r3 = previous.
[[nodiscard]] bool ke_set_disable_boost_thread_export(xenon::kernel::KernelProcess& process,
                                                      xenon::core::ExportCallContext& context);
// FscGetCacheElementCount (0x20) / FscSetCacheElementCount (0x21).
[[nodiscard]] bool fsc_get_cache_element_count_export(xenon::kernel::KernelProcess& process,
                                                      xenon::core::ExportCallContext& context);
[[nodiscard]] bool fsc_set_cache_element_count_export(xenon::kernel::KernelProcess& process,
                                                      xenon::core::ExportCallContext& context);

// --- Process-free (registered directly) --------------------------------------
// InterlockedPushEntrySList (0x2D), InterlockedPopEntrySList (0x2C),
// InterlockedFlushSList (0x2B): a lock-free singly-linked stack whose 8-byte
// header (next pointer, depth, sequence) is updated with a real 64-bit
// load-reserve/store-conditional loop on guest memory, so it interoperates with
// the many titles that inline the same operation. NtYieldExecution (0x101),
// KfRaiseIrql (0xB2) and KeEnableFpuExceptions (0x5D) complete the set.
[[nodiscard]] bool register_xboxkrnl_threading_exports(xenon::core::ExportRegistry& registry);

}  // namespace xenon::xbox
