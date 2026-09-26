# Kernel-Object Liveness Accounting — reviewer addition 3 (RunFingerprint half already done)

Reviewer feedback addition 3 on the AC6 Runtime Readiness pass asked for
"RunFingerprint + kernel-object liveness accounting". `RunFingerprint`
itself (`include/xenon/core/run_fingerprint.hpp`,
`XenonSession::capability_report()`'s `"runFingerprint"` section) has
existed since Phase 0 of this pass. The liveness half did not exist until
this follow-up.

## A real bug found while building it

`kernel::ThreadManager::thread_count()` already existed and looked like
exactly the right primitive - until checking who calls
`ThreadManager::remove_thread()` (also already implemented) turned up
**nobody**. Every guest thread ever created via `ExCreateThread`, and the
main thread itself, stayed in `ThreadManager::threads_` forever after
finishing. `thread_count()` was silently reporting "threads ever created
this session," not "threads currently live" - a session that created and
finished many short-lived guest threads (a very ordinary pattern for real
titles) would report an ever-growing count with no way to tell a leak from
normal churn. Reporting this number as "kernel-object liveness" without
fixing it first would have been exactly the kind of silently-wrong
telemetry CLAUDE.md prohibits - worse than not reporting anything, since
it looks like real liveness data.

## The fix

- `XenonSession::run_created_guest_thread()` (`src/core/session.cpp`): now
  calls `thread_manager().remove_thread(thread->thread_id())` once the
  thread's dispatch has genuinely ended, mirroring the existing
  `exception_dispatcher_.clear_thread_handlers()` cleanup right above it.
  Safe to do from within the thread's own `ThreadEntry` closure: the guest
  handle (`HandleTable`) and this function's own `thread` shared_ptr keep
  the `KernelThread` object itself alive - this only stops the scheduler
  from tracking a thread that will never run guest code again.
- `XenonSession::run_execution()` (the main thread's own dispatch): same
  fix, added once its own dispatch outcome is known, before branching on
  which exit path to take.

## What this pass added

A new `"kernelObjects"` `capability_report()` section (omitted until a
kernel process exists, matching `"gpu"`/`"shader"`'s convention):

```
"kernelObjects": {
  "liveThreads": <real ThreadManager::thread_count()>,
  "liveHandles": <real HandleTable::size()>
}
```

A point-in-time snapshot, not a running total - so a leak (a live count
that keeps growing across many created-and-finished guest
threads/objects instead of returning to baseline) is actually observable,
which is the entire point of "liveness" as opposed to a cumulative
counter.

## Tests added

`tests/core/thread_creation_tests.cpp`:
- `test_thread_count_returns_to_baseline_after_threads_finish()` creates
  and joins four guest threads sequentially through the real
  `ExCreateThread` path and asserts `thread_count()` returns to its
  pre-test baseline after every single one - this is exactly the shape the
  bug had (it would have read baseline+1, +2, +3, +4 before the fix).
- `test_capability_report_kernel_objects_section_reflects_real_liveness()`
  asserts `"kernelObjects"` reports `liveThreads == 0` after a created
  thread finishes (removed from `ThreadManager`) while `liveHandles >= 1`
  (its handle is still open - a finished thread's handle stays valid until
  explicitly closed, matching real Xbox 360 semantics), then asserts
  `liveHandles` drops back to `0` once the handle is actually closed.

## Tests run

- `xenon_thread_creation_tests`: all pass, including both new tests.
- Full solution rebuild + full test suite: see this pass's finish report.

## Remaining gaps

Only `ThreadManager`/`HandleTable` totals are surfaced - no per-object-type
breakdown (e.g. how many of the live handles are Thread vs Event vs File
objects). `HandleTable` does not currently expose a per-type count; adding
one was not attempted here since nothing in the reviewer's addition asked
for that granularity specifically, and inventing it without a concrete
consumer would be speculative.
