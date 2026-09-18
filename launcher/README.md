# Xenon Launcher UI

This folder contains the standalone Project Xenon launcher front-end.

The launcher is intentionally isolated from `xenon_core`, CPU, memory, and graphics so Qt remains a user-interface dependency rather than leaking into the runtime ABI or game-module interfaces.

## Current milestone

The current launcher is a front-end-first implementation. Navigation and UI interactions work even where the corresponding Xenon runtime service is not implemented yet.

Implemented front-end behaviour includes:

- Library, Modules, Profiles, and Settings navigation.
- A normal empty-state flow with no preinstalled games, modules, DLC, or copyrighted content.
- Local file-selection dialogs for game content, DLC, and module packages.
- Module-provided tile and hero artwork slots.
- Game information, content state, compatibility, and module status panels.
- Working dropdown menus and front-end action feedback.
- Profile creation, duplication, editing, activation, and deletion in front-end state.
- Four working themes: Xenon Cyan, Xenon Green, Industrial Amber, and Light.
- Persistent theme, active profile name, and last-open page via `QSettings`.
- Toast feedback for actions whose runtime/backend service does not exist yet.

The launcher does **not** download commercial game files or DLC. Game content is expected to be supplied locally by the user and identified/validated by the appropriate game module.

## Test mode

Normal Xenon builds deliberately start with an empty library and module manager. The launcher does not hard-code Project Gracemeria, any commercial game, DLC catalogue, or installed module into the production UI.

For UI development there is a compile-time test mode in:

```text
launcher/src/launcher_config.hpp
```

Change:

```cpp
inline constexpr bool kTestMode = false;
```

to:

```cpp
inline constexpr bool kTestMode = true;
```

and rebuild the launcher.

Test mode injects **fictional** front-end-only data such as `Xenon Test Flight`, test modules, and test add-ons. This exists only so developers can exercise the complete game detail page, list selection, disabled states, menus, DLC status rows, profile switching, and module actions before the real registry/content backends are connected.

A visible `TEST MODE` indicator is shown when the build uses fictional data. Test data is not persisted as real launcher content.

## Build requirements

- CMake 3.25+
- C++20 compiler
- Qt 6.6+ with Quick, Quick Controls 2, and Quick Dialogs 2

The root CMake project enables `XENON_BUILD_LAUNCHER` by default. If Qt is not available, the launcher is skipped without preventing the Xenon runtime from configuring or building.

Example:

```sh
cmake -S . -B build/launcher -DXENON_BUILD_TESTS=OFF
cmake --build build/launcher --target xenon_launcher
```

## Module artwork contract

The production launcher does not embed game artwork. Future module metadata should supply URLs/paths for at least:

- Library tile artwork.
- Selected-game hero/banner artwork.

Xenon displays module-provided assets; game-specific artwork does not belong in the generic launcher source tree.

## Planned data flow

The current QML `ListModel` objects are presentation placeholders only. Normal mode leaves them empty. The intended production flow is:

```text
Module/content registry
        |
        v
C++ launcher models/services
        |
        v
QML presentation models
        |
        v
Library / Modules / Profiles UI
```

Game-specific metadata continues to flow from the game module into Xenon. Xenon must not depend on any individual game project.

## Backend integration

`LauncherBridge` is currently a small seam between QML and C++. Runtime-backed controls call `notifyUnavailable()` until the associated service exists. Future work should replace those stubs with dedicated C++ models and services for:

- Module discovery and manifests.
- Local game-content import and validation.
- DLC catalogue/import state.
- Save/profile storage.
- File locations and folder opening.
- Launch/runtime lifecycle.
- Module/runtime update checks.

Those services should remain generic and should not contain game-specific behaviour.
