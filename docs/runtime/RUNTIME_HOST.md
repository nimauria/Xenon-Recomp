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
  "moduleSettings": {},
  "runtimeApiRequirements": {},
  "nativeExtensionPath": "C:/Xenon/modules/halo3-module/native/halo3.dll",
  "profileId": "profile-1",
  "profileName": "Player One",
  "region": "Auto (Global)",
  "profileXuid": "16140901064495857665",
  "renderer": "Automatic",
  "shaderCache": true,
  "shaderCacheMode": "Persistent",
  "inputBackend": "Automatic",
  "inputPreferredDevice": "Automatic",
  "inputDeadzone": 0.10,
  "inputRumble": true,
  "inputBackground": false,
  "inputModuleApiVersion": 1,
  "inputProfileStorePath": "C:/Xenon/Profiles/input-profiles-v1.conf",
  "inputUserSources": [{ "userIndex": 0, "sources": [] }],
  "audioMasterVolume": 1.0,
  "audioMuteUnfocused": false,
  "audioLatencyProfile": "",
  "logVerbose": false,
  "titleUpdatePath": "",
  "dlcRootPath": "",
  "dlc": [{ "type": "DLC", "path": "C:/Games/Halo3/DLC/map1" }],
  "savePath": "C:/Xenon/saves/halo3",
  "screenshotsPath": "C:/Xenon/screenshots/halo3",
  "offline": true,
  "headlessMode": false
}
```

Only `sessionDir` and `contentPath` are required; everything else has a
sensible empty/default fallback. `nativeExtensionPath` is resolved by the
launcher from the module's manifest (`ModuleService::nativeExtensionPath`,
see below) — the runtime host does not parse module manifests itself.

`contentPath` may be an extracted directory containing a root `default.xex`,
a loose `.xex`, an Xbox 360 `.iso`/`.xgd` GDFX/XDVDFS image, or a `.dvd`
descriptor. `runtime_host/src/content_source.cpp` normalizes all four forms.
Disc images are read and mounted directly without whole-image extraction; a
loose XEX uses its containing directory as the runtime `game:` filesystem.

`headlessMode` (defaults to `false`, i.e. Normal Play) is a Gracemeria
readiness pass addition (Part 5) - see "Normal Play vs. headless/test mode"
below for exactly what relaxes when it is `true`. An ordinary launcher Play
action never sets it; it exists for automated compatibility sweeps and
dedicated/offscreen hosts.

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
  "loadedXex": { "loaded": true, "titleId": "4D53081A", "entryPoint": "82001000", "imageBase": "82000000", "executableRanges": 3, "titleUpdateApplied": false, "baseVersion": "1", "effectiveVersion": "1", "effectiveImageHash": "a1b2c3..." },
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

## Normal Play vs. headless/test mode (Gracemeria readiness pass, Part 5)

Normal Play (the default: `LaunchConfig::headless_mode` is `false` unless a
launch config explicitly sets `"headlessMode": true`) requires required base
content, a requested real presentation window/surface, and audio to all
actually succeed - the runtime host **fails the launch outright** rather than
silently continuing in a degraded state:

- **Content**: if the XEX's title ID can be determined but
  `mount_content_graph()` fails, or the title ID cannot be determined at all,
  the launch fails with `ContentMountFailed`/`MissingRequiredContent`. Only
  `headless_mode` may continue without mounted content.
- **Presentation**: the game window/surface is created *before*
  `session.start()` (so a failure is caught before any guest code ever runs,
  not after). A real graphics backend (`session.gpu() != nullptr`) with no
  usable window fails with `WindowCreationFailed`, never "continues without
  it" - except in `headless_mode`.
- **Audio**: `SessionConfig::enable_audio` is `!launch.headless_mode`; a real
  audio backend that fails to construct already fails `session.initialize()`
  outright (`AudioBackendFailed`) - this was already strict before this pass,
  `headless_mode` is what now lets a launch opt out of requesting audio at
  all.
- **Graphics/input backend selection**: already strict before this pass (see
  "What this does not cover yet" below for presentation itself) - a
  requested-but-unavailable Vulkan/D3D12/XInput/SDL backend fails
  initialization outright; only the literal `"None"` selector produces a
  headless-safe fallback, and `InputSystem` refuses to end up with zero real
  providers unless a driver list explicitly contains `"null"`/`"none"`.

Every fatal launch failure calls `StatusWriter::write_fatal(message, category)`
with a `xenon::runtime_host::LaunchFailureCategory` (`status_writer.hpp`):
`SessionInitFailed`, `MissingRequiredContent`, `ContentMountFailed`,
`GameLoadFailed`, `ModuleRevisionMismatch`, `StartFailed`,
`WindowCreationFailed`, `GraphicsPresentationFailed`, `AudioBackendFailed`,
`InputBackendFailed`. When set, `status.json` carries an `"errorCategory"`
string field alongside the existing free-text `"lastError"`, so a launcher/
dashboard can branch on failure kind without string-matching.

## Stopping a session

Stopping already-running native compiled code cannot be preempted — there
is no interpreter or yield point to suspend at (see
`docs/runtime/RUNTIME_SESSION.md`). The stop contract is therefore cooperative with
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
  `docs/recomp/RECOMPILATION_PIPELINE.md`) that sets `context.compiled_registry`
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

### Effective executable identity and title updates

`titleUpdatePath` (see the schema above) is a search directory, not a
specific file: `XenonSession::mount_content_graph()` hands it to Content
Services (`ContentManager::build_content_graph()`), which discovers,
validates, and selects one compatible title update — the single, canonical
selection for this launch. The runtime host reads that decision back
(`session.content_graph()->selected_title_update`), reads its bytes, and
passes them to `load_game()`'s `title_update_bytes` parameter, which applies
them via XEX Loader V2's `xbox::apply_title_update()` before mapping
anything into memory. The **effective** (possibly title-update-patched)
image — not the base `default.xex` — is what actually gets mapped, import-
resolved, bound to the native extension, and executed. A missing/malformed/
incompatible selected update fails the launch outright (`load_game()`
returns failure, surfaced the same way any other load failure is); there is
never a silent fallback to the base XEX. A launch with no title update
available/selected/enabled continues to run the base XEX unchanged. See
`docs/runtime/RUNTIME_SESSION.md`'s "Title Update Integration" section for the full
mechanism.

The optional `Xenon_SupportedExecutableRevisions()` native-extension export
(see `docs/runtime/RUNTIME_SESSION.md`) lets a module declare which effective-image
SHA1 revision(s) it was compiled for; `load_native_extension()` refuses to
bind a module against a running effective image outside that declared set,
so a module built for the base executable can never silently run against a
title-update-patched one (or vice versa) merely because the title ID
matches. `status.json`'s `loadedXex` object reports the running effective
identity (`titleUpdateApplied`, `baseVersion`, `effectiveVersion`,
`effectiveImageHash`) for diagnostics.

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

- **Presentation (window/swapchain) is now real.** `XenonSession::init_gpu()`
  selects and constructs a real native backend (Vulkan and/or D3D12,
  whichever the build has compiled in) based on `graphics_backend` -
  "Automatic" picks D3D12 on Windows / Vulkan on Linux, an explicit
  "Vulkan"/"D3D12" request is honored or fails initialization outright, and
  "Null"/"None" is the only way to get `gpu::NullBackend` (a deliberate,
  explicit choice, never a silent fallback). `runtime_host/src/
  presentation_host.{hpp,cpp}` now creates a real SDL2 window and wires it
  to that backend's `configure_presentation()` whenever one is active (never
  for `NullBackend`), and the main loop pumps window events (close ->
  orderly `session.stop()`, resize -> `resize_presentation()`, focus ->
  `session.set_focused()`). Not yet covered: fullscreen/borderless toggling,
  and dedicated failure-injection tests for window/surface/swapchain
  creation failure (the existing rollback path is exercised by normal
  shutdown, not by injected mid-initialization failures).
- **Guest process/thread model is now real** (`KernelProcess`/
  `KernelThread`/TLS via a per-thread KPCR - see docs/runtime/RUNTIME_SESSION.md's
  "Now Real" section for the full detail), but the guest export ABI round
  trip is still only verified through `XenonSession::exports()` called
  directly, not through actually-recompiled PPC code reaching it via a real
  import-table call. See docs/runtime/RUNTIME_SESSION.md's "In Progress" section for
  exactly why this was not safe to rush this pass.
- **Cooperative pause/resume.** `pause()`/`resume()` only affect reported
  state; there is no checkpoint in generated code to actually suspend
  execution at.
- **Import resolution completeness.** `resolve_xex_imports()` is a
  diagnostic pass (recorded in `unresolvedImports`), not a hard gate —
  most titles only exercise a fraction of their imports, and static
  recompilation bakes guest-to-guest calls into the generated code directly.
- **Audio settings are real, `audioLatencyProfile` excepted.**
  `XenonSession::init_audio()` now always requests Audio V1 for normal Play
  and fails session initialization if the host backend cannot be created (no
  silent Null fallback). `audioMasterVolume` is applied as a genuine final
  output-gain multiplier in `AudioSystem::render()`. `audioMuteUnfocused` is
  wired to a real `AudioSystem::set_muted()` call, but only reachable through
  the new `XenonSession::set_focused(bool)` entry point - nothing calls it
  automatically yet, since that requires the OS window focus events the
  presentation layer above doesn't have. `audioLatencyProfile` is recorded on
  `SessionConfig` but intentionally not consumed: `AudioBackendConfig::callback_frames`
  is hard-tied to the Xbox render-driver's fixed 256-sample frame contract, so
  a host latency/buffer-size knob cannot be layered on without decoupling
  that guest-visible timing first.
- **`logVerbose` is end-to-end.** The launcher's live
  `developer/verboseLogging` preference is copied into `LaunchConfiguration`,
  serialized by `RuntimeBridge`, parsed by the host, and drives
  `SessionConfig::verbose_logging` diagnostics.
- **Display/window settings are not part of the schema yet.** The runtime now
  owns a real presentation window/surface, but resolution/fullscreen/vsync are
  not exposed as launch-contract controls and therefore still use the host's
  current defaults.
- **Input launch settings are real; XAM/xboxkrnl-file-I/O export
  reachability is real.** `XenonSession::init_input()` selects real SDL/
  XInput drivers via `input_backend` (or fails outright if none can be
  created), applies background-input and global rumble policy, loads the
  launcher-owned input profile store, applies the launcher deadzone to the
  default profile, and resolves preferred/per-user source routing against
  connected device identities. Guest `XamInput*`, XAM, Audio, and xboxkrnl file-I/O
  (`NtCreateFile` et al.) calls all now resolve through the one
  `core::ExportRegistry` `XenonSession::exports()` owns, rather than the
  separate `cpu::ExternalCallRegistry`/`xbox::ImportRegistry` surfaces that
  previously had no session ever registering into them.
- **Guest process/thread startup is integrated.** `XenonSession` creates a
  `KernelProcess`/`KernelThread`, registers the loaded module, provisions the
  guest stack/KPCR/TLS context, and executes the bound compiled-code registry
  through CPU V2. Multiple independent guest threads remain later work.
