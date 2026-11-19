# Xenon framework integration

6 November 2026. The first production integration stage connects the launcher to the existing `xenon_runtime_host` process. It does not claim a successfully running retail game or complete framework integration.

## Connected session path

The launcher writes the host's existing version-1 `launch-config.json`, starts a separate process, reads `status.json` and bounded `log.txt` tails, and requests asynchronous stop through `stop.signal`. Creating a process does not mean a game is Running. The host's matching session ID and recognized state determine the displayed result. A surviving session can be reattached after the launcher closes.

Runtime selection precedence is an explicit `--runtime-host=<absolute executable>`, a saved selection, then `xenon_runtime_host.exe`/`xenon_runtime_host` beside the launcher. **Settings → Runtime** provides selection, validation and reset. Selecting a host never executes it. A missing host stays unavailable; fixture executables are never discovered as a fallback. Safe mode blocks new games and provider changes but can observe/stop an existing session.

Game/module identity, validated native extension path, module settings, runtime API requirements, profile name/region/offline settings, save/capture destinations, renderer, cache, audio and input preferences reach the launch configuration. Input routing uses the host's `inputUserSources`/`userIndex` field names and Input API version 1. Normal Play explicitly uses `headlessMode: false`. The decimal-string offline XUID preserves the legacy host default; signed-in identity is not implemented here.

Fixture mode stays explicit and separate. A test peer that reports `fixture: true` remains labelled as a fixture even through the production transport, and cannot affect real game play counts. Active sessions prevent provider changes and relevant game/module edits, including the host's initializing, ready, loading and paused states.

Windows file reads share read/write/delete access so they can coexist with atomic publication and live logs. Short bounded retries handle transient sharing conflicts; persistent failures remain visible and do not replace authoritative state. File-size bounds are checked on the opened file. See the primary [CreateFile sharing contract](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew) and [replacement operation](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw).

## Running with a framework build

Build the framework separately using its existing runtime preset/target, then select that executable in Settings → Runtime. Keep its runtime dependencies beside it. For an explicit development run from this directory:

```powershell
npm run desktop -- --runtime-host="..\build\windows-x64-release\runtime_host\xenon_runtime_host.exe"
```

`XENON_RUNTIME_HOST` supplies the same development override. Native safe restart preserves the explicit argument. Command-line selection is read-only in Settings; a saved selection is used by subsequent normal launches. Standalone launcher packages still do not bundle the Xenon runtime or preparation tool.

The local normal launcher was configured with the verified host in the current checkout. Its existing games and profiles were compared before/after and preserved. No normal game was started.

## Evidence and limits

- Production web build and 37 frontend tests pass.
- All 22 native suites pass on Windows and Linux, including atomic publication, live log sharing, host selection, failed-selection rollback, duplicate launches, reattachment, stop, safe-mode restrictions and fixture provenance.
- Isolated CEF checks pass for runtime Settings, invalid paths, persistence, four languages, narrow layout, per-game settings, demo installs, profiles, media, shell, services and English/French crash recovery. Evidence: `build/runtime-desktop-checks.log`.
- The actual existing Windows runtime host consumed a normal-Play configuration and returned `MissingRequiredContent` for deliberately invalid XEX data. Its authoritative error/logs, non-fixture classification, saved selection and launcher restart were verified. No Running state or real play count was invented. Evidence: `build/real-runtime-1793995587863/result.json`; tested host SHA-256 `1dba923d76897e5d5f7e466e3ec59656c9792053ee01091aa315cc39d8880f95`.
- Reproduce the real-host failure-path check with `npm run test:real-runtime -- <absolute host executable>`. It uses a fresh folder below `build/`; it does not run a user's game. The positive lifecycle tests use an explicitly identified protocol peer, not evidence of guest compatibility.

## Next integration stages

1. Preparation: connect the [existing xenon-prepare workflow](../docs/development/GAME_PREPARATION.md) to persisted jobs, progress, cancellation, cached module registration and first-Play handoff. Build success must remain separate from runtime compatibility.
2. Content: connect authoritative identification/integrity, effective title updates and add-on mounting. Registered DLC references are not proof of live mounts; the current host requires content-graph roots that must be resolved explicitly.
3. Live service management: implement the real input-device/profile, filesystem and network providers. Their management controls remain unavailable independently of session launching. Basic launch preferences are already forwarded.
4. Distribution and acceptance: stage compatible host/tool dependencies, validate actual supported game sessions and stop/restart, and complete Mac/clean-machine/controller/accessibility acceptance. The framework sources currently being edited elsewhere were not modified or rebuilt by this integration pass; the real-host check used the existing binary identified above.

Saved collections, Storage and richer game-detail tabs remain useful later product refinements; they are not prerequisites for these integration stages.
