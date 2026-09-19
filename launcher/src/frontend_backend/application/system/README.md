# Window & System Integration

This feature owns launcher-only desktop integration. It deliberately sits above Xenon runtime/input services.

## Responsibilities

- persistent normal window position/size and maximized-state restore;
- recovery from saved off-screen geometry after monitor/layout changes;
- optional start-minimized policy (suppressed in Safe Mode);
- current-user `xenon://` protocol registration on Windows;
- parsing launcher command-line/deep-link routes;
- forwarding navigation/activation requests to the QML transport layer.

Process ownership is handled by `SingleInstanceService`. A second launcher process sends its arguments to the primary process through a user-scoped `QLocalServer` endpoint and exits before creating recovery state. This prevents two launchers from independently mutating profiles, module state, notifications or session history.

## Supported routes

- `xenon://home`
- `xenon://library`
- `xenon://library/<game-id>`
- `xenon://modules`
- `xenon://modules/<module-id>`
- `xenon://modules/catalog`
- `xenon://profiles`
- `xenon://profiles/<profile-id>`
- `xenon://settings`
- `xenon://settings/<category-id>`

Equivalent developer/automation arguments include `--home`, `--library`, `--modules`, `--profiles`, `--game <id>`, `--module <id>`, `--profile <id>`, `--settings <category>` and `--module-catalog`.

Deep links are navigation-only. They do not directly install modules, launch games, delete data, change settings or execute other mutating command-palette actions.

## Deliberately deferred

System tray behaviour, installer-owned file associations, Windows Jump Lists and controller navigation are separate concerns. Controller navigation should wait for Xenon Input rather than introducing a competing launcher input abstraction.
