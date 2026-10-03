# Launch and session lifecycle

The launcher treats a game launch as a stateful application workflow rather than a direct QML ->
runtime call.

```text
Library UI
   |
   v
SessionController
   |
   +-- Preparing   -> assemble LaunchConfiguration
   +-- Validating  -> library/module/runtime-boundary validation
   +-- Starting    -> hand validated configuration to RuntimeBridge
   +-- Running     -> live session, timer and activity metadata
   +-- Stopping    -> clean runtime stop request
   +-- Failed      -> typed stage/error data retained until dismissed/retried
   |
   v
LaunchFeature -> LaunchService -> IRuntimeBridge -> Xenon Core
```

## Current runtime boundary

The runtime boundary is process-separated and real, not simulated in-process. `RuntimeBridge`
validates the configuration in `prepareLaunch()`, writes the versioned launch contract, and starts
`xenon_runtime_host` as a separate process. From there it supervises that process's `status.json`
and reports the runtime's actual state back to `SessionController`. A session does not reach
`Running` merely because the host process was created: `SessionController` waits until the runtime
reports `stateName: "running"` in `status.json`, and runtime failure/crash/stop transitions are
propagated back into launcher state as typed `Failed`/`Stopping` outcomes rather than silently
dropping back to `Idle`.

Fixture/test content follows the same rule. A fixture can pass launcher-side validation, but test
content is never reported as an executable game session; test-mode runs use a separate history key
so they do not pollute production session history.

## Cancellation and stop

Preparing/Validating/Starting work is queued through the event loop and tagged with a generation ID.
A cancel request invalidates pending phase callbacks and records a `cancelled` history entry. A
Running session moves to `Stopping` and calls the runtime stop seam. Stop failures become a typed
Failed session rather than silently returning to Idle.

## Session data

The current session projection contains the session/game/profile/module identity, state, timestamps,
elapsed time, launch configuration and a typed error object (`code`, `title`, `message`, `details`).
The bridge exposes this projection globally so Library and the footer can react without knowing
anything about RuntimeBridge implementation details.

The last 50 completed/failed/cancelled sessions are persisted in launcher settings. Test-mode history
uses a separate key so fictional fixture launches do not pollute production history.

## Library activity

A successful runtime start updates `lastPlayedAt` and increments `playCount`. Clean/failed endings
after a real Running state accumulate `totalPlayTimeMs` and record the last session duration/outcome.
Failures before Running are kept in session history but do not count as played time.
