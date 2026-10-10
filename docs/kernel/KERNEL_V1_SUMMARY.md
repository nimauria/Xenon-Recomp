# Kernel V1 Implementation Summary

## Status: Host-side object model COMPLETE; guest export surface PARTIAL

**Build:** xenon_kernel.lib builds successfully
**Files:** 10 new headers, 8 new implementations
**Lines:** ~2,500 lines of code
**Components:** 8 major systems implemented

This document previously claimed "50+ xboxkrnl functions ready for export
registration" under a "COMPLETE" status. That was inaccurate and was
corrected during the AC6 Runtime Readiness / Platform Fidelity pass: the
host-side C++ classes listed below existed and were unit-testable, but until
that pass, **zero** of them were reachable from guest code - `init_exports()`
registered only RTL's `RtlImageXexHeaderField`, XAM, audio, input, and file
I/O. A title could not call `ExCreateThread`, `KeSetEvent`, or any other
export listed here. "Class exists" and "guest-callable" are different claims;
this document now tracks them separately below rather than conflating them.

## Completed Components (host-side object model)

1. **Object System** - KernelObject, HandleTable, extended ObjectType
2. **Thread Management** - KernelThread, ThreadManager, TLS (64 slots)
3. **Synchronization** - Event, Semaphore, Mutant, Timer, Wait functions
4. **Time Services** - System time, performance counters, sleep/delay
5. **Memory Integration** - KernelMemory wrapper for Memory V2
6. **Module System** - KernelModule, ModuleManager, export resolution
7. **Process State** - KernelProcess with thread/module/memory management
8. **Exception Handling** - ExceptionDispatcher, fault conversion

## Export Surface: actually guest-callable today

Registered in `XenonSession::init_exports()` (`src/core/session/exports/export_registration.cpp`) and
verified by tests driving them through the real `core::ExportRegistry`, not
just claimed:

- **Time** (`src/xbox/exports/xboxkrnl_time_exports.cpp`, 4 exports):
  `KeQueryPerformanceFrequency` (0x83), `KeQuerySystemTime` (0x84),
  `KeDelayExecutionThread` (0x5A), `KeStallExecutionProcessor` (0xA8).
  Tests: `tests/xbox/time_export_tests.cpp`.
- **Synchronization, handle-based (`Nt*`) only**
  (`src/xbox/exports/xboxkrnl_sync_exports.cpp`, 7 exports):
  `NtCreateEvent` (0xD1), `NtCreateSemaphore` (0xD5), `NtReleaseSemaphore`
  (0xF3), `NtCreateMutant` (0xD4), `NtReleaseMutant` (0xF2),
  `NtWaitForSingleObjectEx` (0xFD), `NtWaitForMultipleObjectsEx` (0xFE).
  Tests: `tests/xbox/sync_export_tests.cpp`.
- **`ExCreateThread`** (`src/core/session/threading/thread_creation.cpp`'s `export_ex_create_thread()`,
  ordinal 0x0D): spawns a real guest-executing thread, honoring
  `CREATE_SUSPENDED`, publishing its `Handle` through the same shared
  dispatcher-object table the `Nt*` sync exports use (so it is waitable via
  `NtWaitForSingleObjectEx` like any other kernel object), and is
  preemptible via `suspend()`/`terminate()` at compiled-block-boundary
  safepoints, not just at a wait/sleep of its own. Required generalizing
  `run_execution()`'s dispatch loop into a shared, thread-parameterized
  primitive, `XenonSession::dispatch_guest_thread()` — see "Threading/
  synchronization correctness pass" below for the full design and why it was
  previously deferred. Tests: `tests/core/thread_creation_tests.cpp`.
- **`Ke*` kernel-mode synchronization exports**
  (`include/xenon/xbox/xboxkrnl_ke_sync_exports.hpp`): `KeSetEvent`,
  `KeResetEvent`, `KeReleaseSemaphore`, `KeWaitForSingleObject`,
  `KeWaitForMultipleObjects`, `KeInitializeEvent`, `KeInitializeSemaphore`.
  These operate on a raw guest-memory `DISPATCHER_HEADER`-based object
  (`KEVENT`/`KSEMAPHORE`) addressed by guest pointer rather than a `Handle` —
  see "Threading/synchronization correctness pass" below for the resolver
  design. Mutant is deliberately not supported by this resolver (see that
  section). Tests: `tests/xbox/ke_sync_export_tests.cpp`.
- **Timers** (`src/xbox/exports/xboxkrnl_sync_exports.cpp`, 3 exports):
  `NtCreateTimer` (0xD7), `NtCancelTimer` (0xCD), `NtSetTimerEx` (0xFA),
  backed by a real timer-dispatch thread (`kernel::TimerManager`, owned per
  `KernelProcess`) - a timer with a nonzero due time now actually fires
  (including periodic re-arm), where previously `KernelTimer::set()` only
  ever signaled immediately for `due_time == 0`. `NtSetTimerEx`'s guest
  callback routine parameter is accepted but deliberately not invoked (see
  "Threading/synchronization correctness pass" below); the timer object
  itself still fires and is waitable. Tests:
  `tests/kernel/timer_dispatch_tests.cpp`, `tests/xbox/sync_export_tests.cpp`.

All ordinals above were verified against the xenia-project/xenia xboxkrnl
export table (`xboxkrnl_table.inc`) before registration - not guessed - per
this project's own lesson from the `RtlImageXexHeaderField` (ordinal 0x12B)
wrong-ordinal incident.

## Export surface: still NOT guest-callable (known gaps, not silently assumed done)

- **Memory** (`NtAllocateVirtualMemory`, `MmAllocatePhysicalMemory`, etc.),
  **Module** (`XexGetModuleHandle`, etc.), **Process**
  (`PsGetCurrentThread`/`PsTerminateProcess` - not confirmed to exist as real
  Xbox 360 xboxkrnl exports; not found in the reference ordinal table used
  above), and **Exception** (`RtlRaiseException`, etc.) exports remain
  unregistered.

## Documentation

- `docs/kernel/KERNEL_V1.md` - Comprehensive architecture and API reference
- `docs/kernel/KERNEL_V1_SUMMARY.md` - This summary

## Next Steps

1. Register the remaining Memory/Module/Process/Exception exports, each
   ordinal-verified against a real reference table before registration.
2. Any-of multi-wait is still a 1ms polling loop (`wait_for_multiple_objects`);
   not replaced with a real any-of wait mechanism yet.

## Threading/synchronization correctness pass

A follow-up "AC6 Runtime Readiness / Platform Fidelity" pass fixed several
host-side object-model bugs found while building `ExCreateThread`, and
resolved three items that were previously tracked as known gaps:

**Bugs fixed** (all regression-tested in `tests/kernel/thread_lifecycle_tests.cpp`,
`tests/kernel/synchronization_tests.cpp`, `tests/xbox/sync_export_tests.cpp` and
`tests/core/thread_creation_tests.cpp`):

- `KernelMutant` hardcoded `owner_thread_id_ = 1`; it now takes the real
  calling thread's id from `ExportCallContext::thread_id`.
- `wait_on_object()`/`wait_for_single_object()`/`wait_for_multiple_objects()`
  hardcoded the waiting thread's id to 0 for `ObjectType::Mutant`; all three
  now take an explicit `waiting_thread_id`.
- `XenonSession::external_call()` — the real production export-dispatch
  path — hardcoded `ExportCallContext::thread_id` to 0 regardless of the
  actual calling guest thread, which meant the two fixes above only worked
  in isolated tests that construct `ExportCallContext` directly. Fixed by
  resolving `kernel_process_->thread_manager().current_thread()`.
- `join(timeout_ms)` always blocked unconditionally; fixed with a
  `std::promise`/`std::shared_future` completion signal so timeouts are real
  and safe for multiple concurrent waiters.
- `thread_main()` could overwrite an exit code `terminate()` had already set
  concurrently; fixed with a `state_ != Terminated` guard.
- `~KernelThread()`/`ThreadManager::shutdown()` joined unconditionally,
  able to hang teardown forever on a wedged guest thread; both now use a
  bounded (5s) `join(timeout)` with detach-on-timeout.
- `CREATE_SUSPENDED` was unsupported; `thread_main()` now parks before
  calling `entry_()`, reusing the `suspend()`/`resume()` mechanism.

**`ExCreateThread` dispatch design:** `run_execution()`'s dispatch loop
(previously hardcoded to `main_cpu_state_`/`main_thread_`) was extracted into
`XenonSession::dispatch_guest_thread(CpuState&, GuestAddress entry,
const shared_ptr<KernelThread>&)`, a shared, thread-parameterized core used by
both the main thread and every `ExCreateThread`-spawned thread.
`dispatch_guest_thread()` checks `thread->wait_while_suspended()` and
`thread->is_terminated()` at every compiled-block boundary (not
per-instruction), so `suspend()`/`terminate()` from another host thread are
honored within roughly one block's latency for any executing guest thread.
When a thread is terminated mid-dispatch, `thread->exit_code()` (the value
`terminate()` committed) is used as the authoritative exit code rather than
whatever the `CpuState` registers held mid-flight.

**`Ke*` resolver design:** the `Ke*` exports operate on a raw
`DISPATCHER_HEADER`-based guest-memory object (`KEVENT`/`KSEMAPHORE`), not a
`Handle`. `resolve_dispatcher_object()` lazily creates a host-side kernel
object for a guest address on first use and stashes its `Handle` in the
header's own unused `wait_list_flink`/`wait_list_blink` fields (independently
verified against xenia-project/xenia's `xobject.cc`
`GetNativeObject`/`StashHandle` approach, not copied from it), so a repeat
call on the same address resolves the same host object in O(1). Mutant
(`DISPATCHER_HEADER` type 2) is deliberately **not** supported by this
resolver: real `KMUTANT` has extra fields (owner-thread pointer,
abandoned/APC-disable bytes) whose exact Xbox 360 offsets are not
independently verified, and `KeWaitForSingleObject`/`KeWaitForMultipleObjects`
return a diagnosable "unsupported type" error for it rather than guessing.

**Timer callback rationale:** `NtSetTimerEx`'s guest callback ROUTINE
parameter is accepted but deliberately not invoked — real Xbox 360 timer
callbacks run in a DPC/APC context Xenon does not model, and invoking one
would need its own guest-code-invocation machinery plus a defensible "current
thread"/TLS answer that has no verified reference yet. A nonzero routine
pointer is logged as a diagnostic (`xenon::logging::Logger`, category
`"timer"`) rather than silently dropped; the timer itself still fires and
signals correctly for `NtWaitForSingleObjectEx`-style waiters.

**Per-thread exception dispatch:** `kernel::ExceptionDispatcher` now supports
a handler chain scoped to one guest thread id
(`register_thread_handler()`/`clear_thread_handlers()`), tried before the
process-wide chain. This resolves the gap at the layer Xenon's exception
dispatch actually operates at (a host-side C++ handler chain) — it
deliberately does not walk a real guest `EXCEPTION_REGISTRATION_RECORD`/SEH
chain rooted in guest memory, which is separate exception/EH/setjmp/longjmp
work requiring its own verified guest-memory layout. Regression-tested in
`tests/kernel/exception_tests.cpp`.

**Known, still-open gaps:** any-of multi-wait
(`wait_for_multiple_objects(wait_all=false)`) remains a 1ms polling loop, and
Memory/Module/Process/Exception exports (`NtAllocateVirtualMemory`,
`MmAllocatePhysicalMemory`, `XexGetModuleHandle`, `RtlRaiseException`, etc.)
remain unregistered — `PsCreateSystemThreadEx`, `PsTerminateProcess` and
`PsGetCurrentThread` were specifically checked against the reference ordinal
table and were **not found** there, so their existence as real Xbox 360
xboxkrnl exports needs independent confirmation before registration.

---
**Date:** 2026-09-25
**Implementation:** Host-side object model complete; guest export surface
partial (15 of the ~50 originally claimed exports are actually
guest-callable and verified by tests as of this update, including
`ExCreateThread` with preemptive suspend/terminate safepoints and a real
timer-dispatch thread).
