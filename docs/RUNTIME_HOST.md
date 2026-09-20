# Xenon Runtime Host

## Overview

`xenon_runtime_host` is the only process that ever creates a `XenonSession`
and executes guest code. The launcher (`xenon_launcher`) never runs a game
itself — pressing Play spawns a detached `xenon_runtime_host` process and
supervises it entirely through files in a per-session directory. This keeps
a running game alive if the launcher exits or crashes ("runtime
separation"), and keeps Qt out of the runtime's dependency graph (see
`docs/architecture/PROJECT_STRUCTURE.md`).

```text
Launcher (RuntimeBridge)                 xenon_runtime_host
        |                                        |
        | write launch-config.json               |
        | QProcess::startDetached -------------->|
        |                                        | XenonSession::initialize()
        |                                        | mount_content_graph()
        |                                        | load_game()
        |                                        | start()  (spawns guest
        |                                        |           execution thread)
        | poll status.json <---------------------| write status.json (~4/s)
        | tail log.txt      <--------------------| append log.txt
        | touch stop.signal --------------------># session.stop() + grace
        |                                        | period, then exit
```

There is no other IPC channel. No sockets, no shared memory, no COM/DBus.
This keeps the contract simple to implement from either side and easy to
inspect by hand while debugging (the session directory is just files).

## Session directory

The launcher creates one directory per Play action, typically under
`AppDataLocation/runtime-sessions/<uuid>/`, containing:

| File | Writer | Reader | Purpose |
|------|--------|--------|---------|
| `launch-config.json` | Launcher | Runtime host | One-shot input, read once at startup |
| `status.json` | Runtime host | Launcher | Polled snapshot of session state |
| `log.txt` | Runtime host | Launcher | stdout/stderr of the runtime host and `XenonSession`, appended |
| `stop.signal` | Launcher | Runtime host | Presence requests a stop; content ignored |

`status.json` is published atomically (written to `status.json.tmp` then
renamed over `status.json`) so a launcher poll never observes a
half-written file.

## Launch configuration version

`configVersion` is the schema version of the JSON document below (currently
`1`). The launcher always writes it; the runtime host defaults a missing
value to `1` for backward compatibility with older launcher builds, but
rejects a `configVersion` newer than it supports (exit code `2`, before
`XenonSession` is touched) rather than guessing at fields it does not
understand. Bump this only when a field's meaning changes incompatibly -
adding a new optional field with a safe default does not require a bump.

## Launch configuration schema

Written once by the launcher before starting the process, passed as
`--launch-config <path>`:

```json
{
  "configVersion": 1,
  "sessionId": "1d2c...",
  "sessionDir": "C:/Users/.../runtime-sessions/1d2c...",
  "gameId": "halo3",
  "title": "Halo 3",
  "contentPath": "C:/Games/Halo3",
  "moduleId": "org.example.halo3-module",
  "moduleName": "Halo 3 Module",
  "modulePath": "C:/Xenon/modules/halo3-module",
  "moduleVersion": "1.2.0",
  "nativeExtensionPath": "C:/Xenon/modules/halo3-module/native/halo3.dll",
  "profileId": "profile-1",
  "profileName": "Player One",
  "region": "Auto (Global)",
  "profileXuid": "16140901064495857665",
  "renderer": "Automatic",
  "inputBackend": "Automatic",
  "audioMasterVolume": 1.0,
  "audioMuteUnfocused": false,
  "audioLatencyProfile": "",
  "logVerbose": false,
  "titleUpdatePath": "",
  "dlcRootPath": "",
  "dlc": [{ "type": "DLC", "path": "C:/Games/Halo3/DLC/map1" }],
  "savePath": "C:/Xenon/saves/halo3",
  "screenshotsPath": "C:/Xenon/screenshots/halo3",
  "offline": true
}
```

Only `sessionDir` and `contentPath` are required; everything else has a
sensible empty/default fallback. `nativeExtensionPath` is resolved by the
launcher from the module's manifest (`ModuleService::nativeExtensionPath`,
see below) — the runtime host does not parse module manifests itself.

## Status schema

Rewritten continuously (roughly 4 times/second) while the process is alive:

```json
{
  "available": true,
  "pid": 12345,
  "sessionId": "1d2c...",
  "gameId": "halo3",
  "title": "Halo 3",
  "moduleId": "org.example.halo3-module",
  "state": 4,
  "stateName": "running",
  "initialized": true,
  "running": true,
  "executionActive": true,
  "lastError": "",
  "nativeExtension": { "path": "...", "bound": true, "error": "" },
  "unresolvedImports": [{ "library": "xboxkrnl", "symbol": "NtSomething", "ordinal": 210 }],
  "loadedXex": { "loaded": true, "titleId": "4D53081A", "entryPoint": "82001000", "imageBase": "82000000", "executableRanges": 3 },
  "subsystems": { "memory": true, "filesystem": true, "input": true, "gpu": true, "xam": true },
  "startedAtEpochMs": 1732000000000,
  "updatedAtEpochMs": 1732000004200
}
```

`stateName` mirrors `xenon::core::SessionState`: `uninitialized`,
`initializing`, `ready`, `loading`, `running`, `paused`, `stopping`,
`stopped`, `failed`. A launcher UI builds "game status" (module version,
recompilation state, renderer, compatibility warnings) directly from this
file rather than talking to `XenonSession` itself, which never crosses the
process boundary.

If the process cannot even initialize (missing content, bad launch config),
it writes a minimal status with `stateName: "failed"` and `lastError` set,
then exits non-zero shortly after so the launcher can surface the specific
failure instead of a generic "runtime did not start".

### Detecting a crash

`stateName: "crashed"` (`state: -1`) never comes from the runtime host
itself - it is synthesized by `RuntimeBridge` (`launcher/src/runtime/
runtime_bridge.cpp`) when it observes that the runtime host's OS process has
exited without `status.json` ever reaching a terminal state (`stopped` /
`failed`). This covers a hard crash (access violation, `std::terminate`, an
external kill) that never gets a chance to write a final status of its own.

`RuntimeBridge` checks process liveness by pid on every status read
(`RuntimeBridge::queryProcessState`) rather than assuming "no recent status
update" means dead, since a session can legitimately go quiet between the
~250 ms status-write interval. On Windows this also recovers the real exit
code (`GetExitCodeProcess`) and reports it as `exitCode`; on POSIX the
runtime host is not a child process of the launcher (it runs fully detached,
see "Session directory" above), so only liveness (`kill(pid, 0)`), not an
exit code, can be determined, and `exitCode` is omitted.

## Stopping a session

Stopping already-running native compiled code cannot be preempted — there
is no interpreter or yield point to suspend at (see
`docs/RUNTIME_SESSION.md`). The stop contract is therefore cooperative with
a hard fallback:

1. Launcher creates `stop.signal`.
2. Runtime host notices it (polled every 250 ms), calls
   `XenonSession::stop()` (sets `stop_requested()`), and starts a 5 second
   grace period.
3. If the guest execution thread ends within the grace period (because it
   returned naturally, or because a future native extension checks
   `stop_requested()` at a safe point), the process exits normally.
4. Otherwise the runtime host terminates itself (`std::_Exit`) rather than
   hang. This is safe because it is a dedicated OS process for exactly one
   game — killing it never touches the launcher.

`RuntimeBridge::stop()` writes the signal and waits up to 500 ms for
`status.json` to reach a terminal state before returning, so a fast/clean
stop is reflected immediately in the UI; a slow stop is still in progress
when the call returns and finishes asynchronously.

## Native extension contract (module compiled code)

Xenon-Recomp ships no games. A module (e.g. Project Gracemeria) supplies
game-specific metadata, symbol maps, patches, and — for the game to actually
run — the recomp-driver-generated compiled-code registry, built as a shared
library:

- Windows: `.dll`, Linux: `.so`, macOS: `.dylib`.
- Must export one C-linkage symbol:

  ```cpp
  extern "C" void Xenon_BindCompiledRegistry(xenon::cpu::ExecutionContext& context);
  ```

  This is a thin wrapper around the generated `registry.cpp`'s
  `bind_compiled_registry(ExecutionContext&)` (see
  `docs/RECOMPILATION_PIPELINE.md`) that sets `context.compiled_registry`
  and `context.compiled_lookup`.
- Built with the same toolchain/ABI as the paired `xenon_runtime_host`
  build, since the exported function takes the project's real C++ types by
  reference across the library boundary (this is a single-ecosystem
  contract, not a stable cross-compiler ABI).

`XenonSession::load_game()` loads this library (if
`SessionConfig::native_extension_path` is set) and resolves
`Xenon_BindCompiledRegistry` immediately, recording success/failure as
`native_extension_bound()` / `native_extension_error()`. `start()` refuses
to run without a bound registry — it reports the specific reason (missing
module, load failure, missing export) instead of silently doing nothing.
The registry is actually bound into a live `ExecutionContext` once per
guest-execution-thread start, and the entry point is looked up and invoked
through `context.lookup_compiled(entry, CompiledLookupKind::Call)`.

### Module manifest field

The launcher resolves `nativeExtensionPath` from the module's
`xenon-module.json`/`module.json`/`manifest.json` via
`ModuleService::nativeExtensionPath()`:

```json
{
  "id": "org.example.halo3-module",
  "nativeExtension": "native/halo3.dll"
}
```

or, for multi-platform modules:

```json
{
  "nativeExtension": {
    "windows-x64": "native/win64/halo3.dll",
    "linux-x64": "native/linux64/halo3.so"
  }
}
```

A module with no `nativeExtension` entry loads fine (content mounts,
imports resolve) but cannot start — `status.json`'s `nativeExtension.error`
explains why, and the launcher should surface that rather than a generic
launch failure.

## Launcher window behavior after launch

The `runtime/afterLaunch` setting ("Keep launcher open" / "Minimize
launcher" / "Close launcher") controls only the launcher's own window once
`SessionController` reaches `Running`; it never reaches the runtime host or
`LaunchConfiguration` because the game process's lifetime does not depend on
it either way (see "Process independence" above). "Close launcher" quits the
Qt application directly; it does not touch the session directory or send a
`stop.signal`, so the game keeps running.

## Testing

`RuntimeBridge::connect()` honors an `XENON_RUNTIME_HOST_PATH` environment
variable, checked before the default "next to the launcher executable"
lookup. This lets tests point a `RuntimeBridge` at a specific binary (the
real `xenon_runtime_host`, or a lightweight test double that speaks the same
`launch-config.json`/`status.json`/`log.txt`/`stop.signal` contract) without
depending on build output layout. It is never required for a normal
install. See `launcher/tests/runtime/runtime_bridge_tests.cpp` and
`launcher/tests/fixtures/runtime_host_fixture.cpp`.

## What this does not cover yet

- **Real graphics presentation.** `XenonSession::init_gpu()` always creates
  `gpu::NullBackend` today; renderer selection (Vulkan/D3D12) and window/
  swapchain creation are not implemented. The requested renderer is still
  recorded in the launch config and status for when this lands.
- **Cooperative pause/resume.** `pause()`/`resume()` only affect reported
  state; there is no checkpoint in generated code to actually suspend
  execution at.
- **Import resolution completeness.** `resolve_xex_imports()` is a
  diagnostic pass (recorded in `unresolvedImports`), not a hard gate —
  most titles only exercise a fraction of their imports, and static
  recompilation bakes guest-to-guest calls into the generated code directly.
- **Audio settings are recorded, not yet actionable.** `audioMasterVolume`/
  `audioMuteUnfocused`/`audioLatencyProfile` are parsed into `LaunchConfig`
  (forwarded from the launcher's existing `LaunchConfiguration` fields), but
  `SessionConfig`/`XenonSession` have no audio subsystem to hand them to yet
  (no `init_audio()` exists) — the same "recorded but not wired to a real
  backend" pattern as `renderer` above.
- **`logVerbose` is not yet sourced from a live setting.** The launcher
  already has a `developer/verboseLogging` preference (`launcher/src/main.cpp`),
  but `RuntimeBridge` has no `SettingsService` access to read it, so
  `launch()` always writes `logVerbose: false`. The field is fully parsed and
  available to `build_session_config()`; wiring the real setting value
  through is a small follow-up, not a design gap.
- **Display/window settings are not part of the schema.** Resolution,
  fullscreen, and vsync have no consumer anywhere yet — there is no
  swapchain/presentation layer to configure (see "Real graphics presentation"
  above) — so no placeholder fields were added for them.
