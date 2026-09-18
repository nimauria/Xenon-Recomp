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

`RuntimeBridge::prepareLaunch()` is real and validates the configuration/runtime connection. Xenon
Core does not yet expose a generic executable/session start API, so `RuntimeBridge::launch()` still
returns `Runtime session API pending` after successful preparation. `SessionController` therefore
reaches `Starting` and then honestly enters `Failed` today; it never reports a fake Running state.
When Xenon Core implements the session API, the same controller will enter `Running` automatically
when the bridge returns success.

Fixture/test content follows the same rule. A fixture can pass launcher-side validation, but test
content is never reported as an executable game session.

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
