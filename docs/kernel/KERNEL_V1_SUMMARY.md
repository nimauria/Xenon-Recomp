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

Registered in `XenonSession::init_exports()` (`src/core/session.cpp`) and
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
- **`ExCreateThread`** (`src/core/session.cpp`'s `export_ex_create_thread()`,
  ordinal 0x0D): spawns a real guest-executing thread, honoring
  `CREATE_SUSPENDED`, publishing its `Handle` through the same shared
  dispatcher-object table the `Nt*` sync exports use (so it is waitable via
  `NtWaitForSingleObjectEx` like any other kernel object), and is
  preemptible via `suspend()`/`terminate()` at compiled-block-boundary
  safepoints, not just at a wait/sleep of its own. Required generalizing
  `run_execution()`'s dispatch loop into a shared, thread-parameterized
  primitive - see `docs/kernel/THREADING_V2.md` for the full design and why
  it was previously deferred. Tests: `tests/core/thread_creation_tests.cpp`.
- **Timers** (`src/xbox/exports/xboxkrnl_sync_exports.cpp`, 3 exports):
  `NtCreateTimer` (0xD7), `NtCancelTimer` (0xCD), `NtSetTimerEx` (0xFA),
  backed by a real timer-dispatch thread (`kernel::TimerManager`, owned per
  `KernelProcess`) - a timer with a nonzero due time now actually fires
  (including periodic re-arm), where previously `KernelTimer::set()` only
  ever signaled immediately for `due_time == 0`. `NtSetTimerEx`'s guest
  callback routine parameter is accepted but not invoked (see
  `docs/kernel/THREADING_V2.md`); the timer object itself still fires and is
  waitable. Tests: `tests/kernel/timer_dispatch_tests.cpp`,
  `tests/xbox/sync_export_tests.cpp`.

All ordinals above were verified against the xenia-project/xenia xboxkrnl
export table (`xboxkrnl_table.inc`) before registration - not guessed - per
this project's own lesson from the `RtlImageXexHeaderField` (ordinal 0x12B)
wrong-ordinal incident.

## Export surface: still NOT guest-callable (known gaps, not silently assumed done)

- **The `Ke*` (kernel-mode) synchronization variants** - `KeSetEvent`,
  `KeResetEvent`, `KeWaitForSingleObject`, `KeWaitForMultipleObjects`. On
  real Xbox 360, these operate on a raw `KEVENT`/`KMUTANT`/...
  (`DISPATCHER_HEADER`-based) object embedded directly in guest memory as
  part of the game's own data structures, addressed by guest pointer - not a
  Handle. Xenon's `KernelEvent`/`KernelSemaphore`/`KernelMutant` are
  host-side objects reached only through `kernel::HandleTable`. Supporting
  the `Ke*` variants correctly requires modeling that guest-memory layout and
  correlating it with a host object; this was intentionally not attempted
  without a verified reference for the exact structure layout, rather than
  guessed at.
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

1. Model the `Ke*` guest-memory dispatcher-object layout (or confirm it is
   out of scope for the titles Xenon targets).
2. Register the remaining Memory/Module/Process/Exception exports, each
   ordinal-verified against a real reference table before registration.
3. Any-of multi-wait is still a 1ms polling loop (`wait_for_multiple_objects`);
   not replaced with a real any-of wait mechanism in this pass.
4. Per-thread exception-handler chains (currently one session-wide
   dispatcher reused generically - see `docs/kernel/THREADING_V2.md`).

---
**Date:** 2026-09-25
**Implementation:** Host-side object model complete; guest export surface
partial (15 of the ~50 originally claimed exports are actually
guest-callable and verified by tests as of this update, including
`ExCreateThread` with preemptive suspend/terminate safepoints and a real
timer-dispatch thread).
