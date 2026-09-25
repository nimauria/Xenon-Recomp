# Boot Phase Checkpoints — AC6 Runtime Readiness pass, Part 15

`xenon::core::BootCheckpointTracker` (`include/xenon/core/boot_checkpoints.hpp`)
is an idempotent, thread-safe, insertion-order-preserving record of which
platform-level boot milestones a session has reached, so a stalled or
crashed run makes it obvious where real progress actually stopped.
`XenonSession::boot_checkpoints()` exposes it directly; `capability_report()`
publishes it unconditionally as the `"boot"` section's `"reached"` array
(unlike `"gpu"`/`"shader"`, which are omitted when no GPU backend exists -
a boot-progress report is meaningful even before a title is loaded).

## Scope: platform-observable only, never game-state

The pass's instructions are explicit that boot checkpoints must never
hardcode game-state addresses or detect title-specific screens. Twelve
platform-level checkpoints are defined
(`XEX_LOADED`/`ENTRY_STARTED`/`FIRST_GUEST_THREAD`/`FIRST_FILE_OPEN`/
`FIRST_INPUT_POLL`/`FIRST_AUDIO_CLIENT`/`FIRST_GPU_SUBMISSION`/`FIRST_SHADER`/
`FIRST_RESOLVE`/`FIRST_PRESENT`/`FIRST_VBLANK`/`PROFILE_READY`/
`SAVE_ENUMERATION`). `TITLE_SCREEN`/`MISSION_LOAD_BEGIN`/
`MISSION_LOAD_COMPLETE` from the plan's own example list are deliberately
**not** included here - those are inherently title-visible game state, and
the pass's own instructions say Xenon core must never detect that kind of
thing generically. A game module that wants to report a milestone like that
needs its own, separate channel.

## What is actually wired up in this pass

Only three checkpoints are reached by real production code today, each
verified via a real production code path (not just unit-testing the tracker
in isolation):

- `XEX_LOADED` - `XenonSession::load_game()`'s success path.
- `ENTRY_STARTED` - `XenonSession::run_execution()`'s start (before
  `dispatch_guest_thread()` for the main thread).
- `FIRST_GUEST_THREAD` - `export_ex_create_thread()`'s success path (a
  guest `ExCreateThread` call actually succeeded).

Each call logs one `"boot"`-category, `Level::Info` entry via
`xenon::logging::Logger` the first time it is reached.

## Known, tracked gap

The remaining nine defined checkpoints (`FIRST_FILE_OPEN` through
`SAVE_ENUMERATION`) are declared (so `capability_report()`'s `"boot"`
section and `to_string()` already have stable names for them) but nothing
calls `reach_boot_checkpoint()` for them yet - each needs its own hook at
the right production call site (first real `KernelIoManager` open, first
input poll, first `AudioSystem` client, first `Backend::begin_submission()`,
first successful shader load, first resolve, first `present()`, first
vblank/frame-pacing tick, profile-store readiness, save enumeration). This
is a real, honestly-reported gap, not something to fill with a fabricated
"reached" state.

`FIRST_GUEST_THREAD`'s wiring is exercised in code review but has no
automated regression test today: doing so needs a synthetic guest XEX whose
entry actually calls `ExCreateThread`, which none of the existing hand-built
test fixtures do (they all call imports directly from the main thread).
`XEX_LOADED`/`ENTRY_STARTED` are both verified end to end in
`tests/core/guest_export_abi_tests.cpp` through the real
`load_game()`/`start()` pipeline.

## Tests

- `tests/core/boot_checkpoint_tests.cpp`: `BootCheckpointTracker` in
  isolation - idempotency, actual-reached-order (not declaration order),
  `reset()`, and `to_string()` for every defined checkpoint.
- `tests/core/session_tests.cpp`: `capability_report()` publishes an empty
  `"boot"` section for a freshly-initialized session with no game loaded.
- `tests/core/guest_export_abi_tests.cpp`: a real `load_game()`+`start()`
  round trip reaches both `XEX_LOADED` and `ENTRY_STARTED`.
