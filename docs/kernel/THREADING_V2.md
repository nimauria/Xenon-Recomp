# Threading/Synchronization V2 — AC6 Runtime Readiness pass

This document tracks the kernel ABI work from the "AC6 Runtime Readiness /
Platform Fidelity" pass. See `docs/kernel/KERNEL_V1_SUMMARY.md` for the full
list of what is and is not guest-callable today. This file records the
correctness fixes made alongside the export work, and the concrete,
unresolved gaps - it deliberately does not claim more than has been verified
by a build and a passing test.

## What this pass fixed (host-side object model)

- **`KernelMutant`'s hardcoded `owner_thread_id_ = 1` placeholder** (was
  `src/kernel/synchronization/mutant.cpp`). `ThreadManager`/`KernelThread`
  allocate guest thread ids starting at 1 (see `thread.cpp`'s
  `g_next_thread_id`), so a freshly constructed initial-owner mutant could
  previously appear pre-owned by an unrelated first thread. Fixed by having
  `KernelMutant`'s constructor take the real owner thread id as a parameter
  instead of hardcoding one; `NtCreateMutant`'s export handler is the only
  production call site that constructs an initially-owned mutant, and it now
  passes the real calling thread's id from `ExportCallContext::thread_id`.
  Regression-tested at both the object level
  (`tests/kernel/synchronization_tests.cpp`) and end-to-end through the real
  export (`tests/xbox/sync_export_tests.cpp`).
- **`wait_on_object()`/`wait_for_single_object()`/`wait_for_multiple_objects()`
  hardcoding the waiting thread's id to 0 for `ObjectType::Mutant`** (was
  `src/kernel/synchronization/wait.cpp`). Every one of these functions now
  takes an explicit `waiting_thread_id` parameter (defaulting to 0 for
  existing non-mutant-aware callers), threaded through from
  `NtWaitForSingleObjectEx`/`NtWaitForMultipleObjectsEx`'s
  `ExportCallContext::thread_id`.

## What this pass added (guest-callable exports)

See `docs/kernel/KERNEL_V1_SUMMARY.md`'s "Export Surface: actually
guest-callable today" section for the authoritative, currently-accurate list
(11 exports: 4 time, 7 handle-based synchronization). Every ordinal was
verified against the xenia-project/xenia xboxkrnl export table before
registration.

## Known, tracked gaps (explicitly incomplete, not silently assumed done)

### `ExCreateThread` / any thread-creation export

Not implemented. A new guest thread needs to run a real AOT/fallback
dispatch loop for its own entry point - architecturally the same thing
`XenonSession::run_execution()` already does for the main thread, and the
same thing `XenonSession::invoke_audio_callback()` does for a single audio
callback invocation. Neither is directly reusable as-is:

- `invoke_audio_callback()` dispatches exactly one call and returns; it does
  not run the `Branch`/`Fallthrough`/`Return`/`Trap`/`Syscall` dispatch loop
  a thread's entire lifetime needs.
- `run_execution()` implements that full loop, but is hardcoded to
  `main_cpu_state_`/`main_thread_` and interleaves it with main-thread-only
  concerns (exception dispatch wiring, adaptive-observation JSONL logging,
  the top-level dispatch cap).

Making `ExCreateThread` real requires factoring `run_execution()`'s dispatch
loop into a primitive parameterized over an arbitrary `CpuState`/
`KernelThread` instead of the main-thread fields, without changing any
observable behavior for the main thread - its own, independently-testable
piece of work (regression risk: the main thread path is exactly where
`run_execution()`'s existing fault handling, exception dispatch, and
adaptive-observation logging all live).

### `Ke*` (kernel-mode) synchronization exports

`KeSetEvent`, `KeResetEvent`, `KeWaitForSingleObject`,
`KeWaitForMultipleObjects` are not implemented. On real Xbox 360, these
operate on a raw `KEVENT`/`KMUTANT`/... object (`DISPATCHER_HEADER`-based)
embedded directly in guest memory as part of the game's own data structures,
addressed by a guest pointer - not a `Handle`. Xenon's
`KernelEvent`/`KernelSemaphore`/`KernelMutant` are host-side C++ objects
reached only through `kernel::HandleTable`. Supporting the `Ke*` variants
correctly requires:

1. A verified `DISPATCHER_HEADER`/`KEVENT`/`KMUTANT` guest-memory layout
   (offsets, type/size tags) for Xbox 360 specifically - not assumed from
   PC Windows NT, which is not guaranteed to match.
2. A mechanism correlating a guest address holding such a structure with a
   host-side kernel object (e.g. lazily constructing one keyed by guest
   address the first time it's touched, and keeping the guest-visible state
   bits and the host object's state consistent with each other).

Neither was attempted in this pass without a verified reference for (1) -
fabricating a struct layout would risk misinterpreting or corrupting real
guest memory silently, which is a materially worse outcome than the export
simply not existing yet.

### Preemptive suspend/terminate safepoints

Tracked separately from the export work above.
`kernel::KernelThread::terminate()`/`suspend()` currently only update state
flags; nothing polls them from inside a running compiled/fallback-executed
guest block, so neither can interrupt a thread actually executing guest
code (only one blocked in a wait/sleep observes state changes promptly, via
the underlying `std::condition_variable`s). A generic safepoint reachable
from both the AOT and fallback dispatch paths (see
`include/xenon/cpu/runtime.hpp`'s `ExecutionContext` and
`XenonSession::run_execution()`) is required; not implemented in this pass.

### Memory / Module / Process / Exception exports

`NtAllocateVirtualMemory`, `MmAllocatePhysicalMemory`, `XexGetModuleHandle`,
`RtlRaiseException`, and similar were not registered. `PsCreateSystemThreadEx`,
`PsTerminateProcess`, and `PsGetCurrentThread` specifically were checked
against the same reference ordinal table used for the exports above and were
**not found** there - before registering anything under those names, their
existence as real Xbox 360 xboxkrnl exports (as opposed to PC-only NT
functions) needs independent confirmation, not assumption from the original
task's function-name list.
