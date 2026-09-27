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

## What is wired up

Three checkpoints are reached at their own direct, non-export production
call site:

- `XEX_LOADED` - `XenonSession::load_game()`'s success path.
- `ENTRY_STARTED` - `XenonSession::run_execution()`'s start (before
  `dispatch_guest_thread()` for the main thread).
- `FIRST_GUEST_THREAD` - `export_ex_create_thread()`'s success path (a
  guest `ExCreateThread` call actually succeeded).

Five more (follow-up to this pass, once `capability_report()`'s Part 17
verdict work was auditing this same area) are reached through
`XenonSession::observe_boot_checkpoint_from_export_call()`, called from
`external_call()` right after a real, successful export dispatch - each
maps a specific, already-verified real ordinal (never guessed) to the
checkpoint it represents:

- `FIRST_FILE_OPEN` - `NtCreateFile` (0x00D2) or `NtOpenFile` (0x00DF),
  `xboxkrnl`.
- `FIRST_INPUT_POLL` - `XamInputGetState` (0x0191), `xam`.
- `FIRST_AUDIO_CLIENT` - `XAudioRegisterRenderDriverClient` (0x1F3),
  `xboxkrnl`.
- `SAVE_ENUMERATION` - `XamContentCreateEnumerator` (0x025C), `xam`.
- `PROFILE_READY` - `XamUserGetSigninState` (0x0210), `xam` - the one
  checkpoint gated on the export's actual return value (a real signed-in
  `xam::SigninState`, not `NotSignedIn`), not merely that the guest asked.

All five reach the checkpoint the moment the export is *dispatched*
(found and handled), regardless of whether the underlying operation itself
succeeds or fails - matching how `FIRST_GUEST_THREAD` doesn't imply the
thread ran bug-free either. A boot checkpoint means "the platform reached
this kind of event," not "this event's own result was correct" (that is
what `capability_report()`'s `"imports"`/`"fallback"`/`"gpu"` sections are
for).

Each call logs one `"boot"`-category, `Level::Info` entry via
`xenon::logging::Logger` the first time it is reached.

## Known, tracked gap

Five remain unwired: `FIRST_GPU_SUBMISSION`, `FIRST_SHADER`,
`FIRST_RESOLVE`, `FIRST_PRESENT`, `FIRST_VBLANK`. Unlike the five closed
above, none of these has a clean production call site to hook: GPU command
submission/shader load/resolve/present all happen inside the GPU backend
(`gpu::Backend`/its `d3d12`/`vulkan` implementations), which by deliberate
design (matching how Parts 7/9/10 of this pass already treat it) has no
reference back to `XenonSession` and is not driven through any
`XenonSession` method; `XenonSession` only ever *reads* the backend's
telemetry (`unsupported_counters()`/`performance_counters()`/
`shader_coverage()`) for `capability_report()`, it never calls into it.

A "poll these counters and infer the checkpoint at
`capability_report()`-call time" approach was considered and rejected:
`capability_report()` is otherwise a pure read of state, and turning it
into something that also *mutates* `BootCheckpointTracker` as a side
effect of being called is architecturally unclean; worse, since it would
observe multiple flipped-since-last-poll checkpoints at once, it cannot
recover their true relative order, which directly undermines
`reached_in_order()`'s whole purpose ("the last entry is the furthest real
progress made"). Wiring these five properly needs either a lightweight
observer callback the GPU backend can report through, or a per-frame
tick on `XenonSession` itself - neither exists today, and inventing one
without a concrete consumer beyond this one checkpoint would be
speculative. Left as a real, honestly-reported gap.

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
- `tests/core/thread_creation_tests.cpp`:
  `test_boot_checkpoints_reached_via_real_export_calls()` drives all five
  export-triggered checkpoints through their exact real ordinal on a fully
  initialized session (not the `ExCreateThread`-only harness elsewhere in
  that file, which bypasses `init_exports()` and has none of these exports
  registered), including a negative case proving an out-of-range/
  never-signed-in user does *not* reach `PROFILE_READY`.
