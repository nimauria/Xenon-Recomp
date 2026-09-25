# Threading/Synchronization V2 — AC6 Runtime Readiness pass

This document tracks the kernel ABI work from the "AC6 Runtime Readiness /
Platform Fidelity" pass. See `docs/kernel/KERNEL_V1_SUMMARY.md` for the full
list of what is and is not guest-callable today. This file records the
correctness fixes made alongside the export work, and the concrete,
unresolved gaps - it deliberately does not claim more than has been verified
by a build and a passing test.

## What this pass fixed (host-side object model)

- **`KernelMutant`'s hardcoded `owner_thread_id_ = 1` placeholder** (was
  `src/kernel/synchronization/mutant.cpp`). Fixed by having `KernelMutant`'s
  constructor take the real owner thread id as a parameter instead of
  hardcoding one; `NtCreateMutant`'s export handler passes the real calling
  thread's id from `ExportCallContext::thread_id`. Regression-tested at both
  the object level (`tests/kernel/synchronization_tests.cpp`) and end-to-end
  through the real export (`tests/xbox/sync_export_tests.cpp`).
- **`wait_on_object()`/`wait_for_single_object()`/`wait_for_multiple_objects()`
  hardcoding the waiting thread's id to 0 for `ObjectType::Mutant`** (was
  `src/kernel/synchronization/wait.cpp`). Every one of these functions now
  takes an explicit `waiting_thread_id` parameter.
- **`XenonSession::external_call()` (the real production dispatch path for
  every guest export call) hardcoding `ExportCallContext::thread_id` to 0
  regardless of which guest thread was actually calling** (was
  `src/core/session.cpp`). Found while building `ExCreateThread`: it meant
  the two fixes above worked correctly in isolated export tests (which
  construct `ExportCallContext` directly) but were never actually exercised
  with real thread identity in production. Fixed by resolving
  `kernel_process_->thread_manager().current_thread()` - the same
  thread-local slot every guest-executing host thread registers itself into
  once at the start of its run. Regression-tested end to end in
  `tests/core/thread_creation_tests.cpp`
  (`test_external_call_resolves_real_calling_thread_identity`).
- **`join(timeout_ms)` always blocked unconditionally regardless of the
  requested timeout.** Fixed with a `std::promise`/`std::shared_future`
  completion signal set exactly once by `thread_main()`, so a timeout is
  real and safe for multiple concurrent waiters (e.g. two guest threads both
  waiting on the same thread handle via `NtWaitForSingleObjectEx`).
- **`thread_main()` unconditionally overwrote `exit_code_`/`state_` with
  `entry_()`'s natural return value, even if `terminate()` had already set a
  different, intentional exit code concurrently.** Fixed with a
  `state_ != Terminated` guard before that overwrite.
- **`~KernelThread()`/`ThreadManager::shutdown()` joined unconditionally,
  able to hang process/session teardown forever on a wedged guest thread.**
  Both now use a bounded (5s) `join(timeout)` with a detach-on-timeout
  fallback in the destructor.
- **No `CREATE_SUSPENDED` support.** `ThreadCreationParams::create_suspended`
  now parks `thread_main()` before it ever calls `entry_()`, using the same
  `suspend_count_`/`suspend_condition_` mechanism `suspend()`/`resume()` use.

All of the above are regression-tested in
`tests/kernel/thread_lifecycle_tests.cpp` and
`tests/kernel/synchronization_tests.cpp`.

## What this pass added (guest-callable exports)

See `docs/kernel/KERNEL_V1_SUMMARY.md`'s "Export Surface: actually
guest-callable today" section for the authoritative, currently-accurate
list. Every ordinal was verified against the xenia-project/xenia xboxkrnl
export table before registration, including `ExCreateThread` (ordinal
0x0D).

### `ExCreateThread` is implemented

A created guest thread runs real guest code (AOT-compiled or
fallback-executed) via the same dispatch primitive the main thread uses.
This required extracting `XenonSession::run_execution()`'s dispatch loop
(previously hardcoded to `main_cpu_state_`/`main_thread_`) into a shared,
thread-parameterized core, `XenonSession::dispatch_guest_thread(CpuState&,
GuestAddress entry, const shared_ptr<KernelThread>& thread)`:

- `run_execution()` is now a thin wrapper: main-thread bookkeeping
  (`execution_active_`, `SessionState` transitions, `set_error()`) around a
  call to `dispatch_guest_thread(*main_cpu_state_, entry, main_thread_)`.
  Verified byte-for-byte behavior-preserving against the existing
  `tests/core/session_execution_tests.cpp` suite (identical log output,
  identical assertions) before and after the extraction.
- `run_created_guest_thread()` is `ExCreateThread`'s `ThreadEntry` body: it
  builds a fresh `CpuState` (own stack, own KPCR/TLS via
  `setup_guest_thread_tls_context()` - the same helper
  `start_audio_guest_thread()` already used), dispatches
  `start_address(start_context)`, and releases the stack/TLS once the
  thread exits (naturally, via crash, or via `terminate()`). A natural
  return's `gpr[3]` becomes the thread's exit code, matching real
  `ExCreateThread`/`ExTerminateThread` semantics.
- `export_ex_create_thread()` (the actual export handler) allocates the
  stack/TLS, creates the `KernelThread` via `kernel_process_->thread_manager()`
  (honoring `CREATE_SUSPENDED` from the guest's creation-flags bit 0x4),
  publishes its `Handle` through `kernel_process_->handle_table()` (the same
  shared table the `Nt*` sync exports use, so a thread handle is waitable
  like any other kernel object via `NtWaitForSingleObjectEx`), and starts
  it.

Solved a genuine chicken-and-egg problem along the way: the `ThreadEntry`
closure needs to know its own `KernelThread`'s id (to look itself up again
via `ThreadManager::get_thread()` and register current-thread identity), but
`ThreadManager::create_thread()` has not returned that `shared_ptr` yet at
the point the closure is constructed. Solved with a small
`shared_ptr<atomic<uint32_t>>` slot filled in immediately after
`create_thread()` returns, strictly before `start()` is called.

Regression-tested in `tests/core/thread_creation_tests.cpp`: thread creation
and `start_context`/exit-code propagation, `CREATE_SUSPENDED`, crash
handling, and the `external_call()` thread-identity fix.

### Preemptive safepoints are implemented

`dispatch_guest_thread()` calls `thread->wait_while_suspended()` (parks if
`suspend()`-ed by another host thread; a cheap lock-free no-op otherwise)
and checks `thread->is_terminated()` (also lock-free) at every
compiled-block boundary (each `Branch`/`Fallthrough` dispatch) - not
per-instruction, which would violate the "cheap in release" constraint.
This means `suspend()`/`terminate()` from another host thread are honored
within roughly one block's latency for **any** guest thread actually
executing code, not only one blocked in a wait/sleep of its own - both for
`ExCreateThread`-spawned threads and the main thread (`main_thread_` is now
passed into `dispatch_guest_thread()` too).

`KernelThread::wait_while_suspended()` and `thread_main()`'s
`CREATE_SUSPENDED` entry-time park share the same mechanism (the latter now
just calls the former), so there is one park/wake implementation, not two.

When a thread is terminated mid-dispatch, `GuestDispatchOutcome::thread_terminated`
tells the caller (`run_execution()`/`run_created_guest_thread()`) to use
`thread->exit_code()` (the value `terminate()` itself committed) as the
authoritative exit code, not whatever the `CpuState`'s registers happened to
hold mid-flight.

Regression-tested in `tests/core/thread_creation_tests.cpp`
(`test_preemptive_safepoint_terminates_a_running_created_thread`,
`test_preemptive_safepoint_suspends_and_resumes_a_running_created_thread`):
a synthetic tight guest loop (branches to itself, incrementing a counter) is
actually interrupted within ~500ms of `terminate()`/`suspend()`, and the
loop is proven to have genuinely stopped dispatching (not merely reported
as stopped while continuing in the background).

## Known, tracked gaps (explicitly incomplete, not silently assumed done)

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
   host-side kernel object.

Neither was attempted without a verified reference for (1) - fabricating a
struct layout would risk misinterpreting or corrupting real guest memory
silently, a materially worse outcome than the export simply not existing
yet.

### Timer dispatch thread / `NtCreateTimer`/`NtCancelTimer`/`NtSetTimerEx` - RESOLVED

`kernel::TimerManager` (`include/xenon/kernel/timer_manager.hpp`, owned per
`KernelProcess`) is a real dispatch thread: it sleeps until the earliest due
time among every `KernelTimer` scheduled with it, fires due timers
(`KernelTimer::fire()`), and re-arms periodic ones automatically.
`KernelTimer` gained `next_due_time()`/`fire()` accessors for this; `set()`
itself is unchanged (`due_time == 0` still fires synchronously inline).
`NtCreateTimer` (0xD7), `NtCancelTimer` (0xCD), and `NtSetTimerEx` (0xFA) -
not `NtSetTimer`, which is not a real xboxkrnl export; the `Ex` suffix is
correct here just as it is for the wait exports - are registered and
tested end to end (`tests/kernel/timer_dispatch_tests.cpp`,
`tests/xbox/sync_export_tests.cpp`: a timer with a real future due time
fires within the expected window and becomes observable as signaled via
`NtWaitForSingleObjectEx`).

`NtSetTimerEx`'s guest callback ROUTINE parameter (a function Xbox 360
titles can ask the kernel to invoke when the timer fires) is accepted but
deliberately **not invoked**: real Xbox 360 timer callbacks run in a
DPC/APC context Xenon does not model, and a dispatch-thread-driven callback
would need its own guest-code-invocation machinery (similar to
`invoke_audio_callback()`) plus a defensible answer for what "current
thread"/TLS context such a callback executes under - neither was built
without a verified reference for the real DPC/APC semantics. A nonzero
routine pointer is logged as a diagnostic (`xenon::logging::Logger`,
category `"timer"`), not silently dropped. The timer object itself still
fires and signals correctly for `NtWaitForSingleObjectEx`-style waiters,
which is the more common usage pattern.

### Any-of multi-wait is still a 1ms polling loop

`wait_for_multiple_objects(wait_all=false)` has not been changed from its
existing polling implementation.

### Memory / Module / Process / Exception exports

`NtAllocateVirtualMemory`, `MmAllocatePhysicalMemory`, `XexGetModuleHandle`,
`RtlRaiseException`, and similar were not registered. `PsCreateSystemThreadEx`,
`PsTerminateProcess`, and `PsGetCurrentThread` specifically were checked
against the same reference ordinal table used for the exports above and were
**not found** there - before registering anything under those names, their
existence as real Xbox 360 xboxkrnl exports (as opposed to PC-only NT
functions) needs independent confirmation, not assumption from the original
task's function-name list.

### Per-thread exception dispatch

`kernel::ExceptionDispatcher` remains a single, session-wide (not
per-thread) dispatcher - reused generically for both the main thread and
created threads' faults/traps via `dispatch_guest_thread()`, which is
correct as far as it goes, but does not give each guest thread its own
independent exception-handler chain. Tracked as Phase 5 of the AC6 Runtime
Readiness plan, not attempted in this pass.
