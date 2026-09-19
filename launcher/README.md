# Xenon Launcher UI

This folder contains the standalone Project Xenon launcher front-end.

The launcher is intentionally isolated from CPU, guest memory, and graphics internals. It may consume narrow public `Xenon::Core` and `Xenon::Filesystem` services through launcher adapters, while Qt remains a launcher-only dependency and never leaks into runtime or game-module interfaces.

## V7 front-end and branding pass

The v7 launcher pass moves the front-end from a functional prototype toward a portfolio-quality desktop application while preserving the backend boundary. The official Xenon branding pack is now embedded as launcher resources; theme-ready SVG assets are recoloured from the active Xenon accent at runtime, while the packaged application icon is used for the Windows executable/window shell.

Key v7 work includes:

- Official Xenon mark/wordmark/lockup resources, theme-aware recolouring, and a bundled Windows application icon.
- One launcher-wide command/search surface across Home, Library, Modules, Profiles, Settings, and launcher actions.
- A simplified primary sidebar with collapse/expand at the top; keyboard shortcuts moved to the Help popover rather than occupying permanent navigation space.
- Create/Edit Profile dialogs with dirty-state protection, outside-click/Escape discard confirmation, immutable generated profile IDs, locally stored user avatars, and optional per-profile content-location overrides.
- A configurable launcher-wide profile storage path. Normal-mode profile state is written atomically to `profiles.json`; profile avatars live under their profile ID.
- A schema-driven Module Settings dialog. Fixture schemas exercise booleans, choices, and strings now; real game modules can later provide an arbitrary manifest-defined settings list without Xenon hard-coding game-specific UI.
- A public module-catalog front-end shell designed for open-source Xenon module packages/metadata only. It never represents commercial games, title updates, assets, or DLC as downloadable Xenon content.
- Developer-only fixture selection (`None`, generic Xenon UI fixtures, and a Project Gracemeria UI preview) when the launcher is built with test mode.
- Library detail panels based on implicit sizing and responsive breakpoints, plus a bounded DLC catalogue with an as-needed scrollbar and isolated wheel/touchpad scrolling.
- Settings categories moved to a top, horizontally scrollable category strip. Search automatically moves to a matching settings category.
- Responsive settings cards and information rows designed to stack rather than clip as launcher text scaling increases toward 200%.
- Separate user-facing About information and developer diagnostics. Developer diagnostics now include OS/kernel, CPU thread count, physical memory, Windows graphics adapter, display/scale state, renderer configuration, fixture state, theme/accessibility state, and backend connection state without dumping local profile/configuration paths.
- Feature gates for module catalog/settings and profile avatars in addition to existing launcher capability gates.

Production behaviour remains clean: a normal build starts with no fictional games or modules installed. Local games and modules appear only after the user imports or installs them.

## Launcher milestone

The completed QML front end now sits on a generic **Launcher Core**. Navigation and presentation remain usable when a deeper Xenon runtime subsystem is unavailable, while production profiles, library metadata, module discovery/state, package staging, paths/settings, and launch-configuration assembly are backend-owned. Controls that depend on framework APIs Xenon does not expose yet fail explicitly rather than silently succeeding.

Current front-end behaviour includes:

- A responsive Library detail layout that keeps title, description, Play/Manage actions, DLC, game metadata, and compatibility information visible at normal desktop sizes.
- An isolated DLC scroll region: mouse-wheel input over the DLC catalogue is consumed by that list and never scrolls the outer game-detail page at the same time.
- Theme-specific decorative backdrops with user-selectable intensity; these are Xenon-owned visuals and remain separate from optional module-provided game artwork.
- A collapsible primary navigation rail with a compact icon-only mode; secondary session/shortcut information lives in contextual surfaces instead of permanently occupying the sidebar.
- A dedicated profile editor dialog for Create/Edit, including profile identity, region/start page, offline behaviour, and optional game/save/screenshot path overrides.
- The profile storage directory is configurable under Settings → Paths. Normal-mode profile state is also written atomically to `profiles.json` there, with QSettings retained as a migration/fallback copy.
- Themed confirmation dialogs replace the unstyled platform/default dialog used by the early prototype.
- Settings switches emit writes only for direct user interaction, preventing programmatic settings refreshes from toggling unrelated accessibility options.
- Application-data/configuration folder actions create missing directories before opening them, so the About-page buttons work on a clean installation.
- The top profile control is now a quick profile switcher only; creation and editing stay on the Profiles page.

- Home dashboard plus Library, Modules, Profiles, and Settings navigation.
- A normal empty library/module state with no bundled game or copyrighted content.
- One clear **Add Game** flow on the Library page for user-provided local content.
- Module import and management on the Modules page rather than duplicating it in Library.
- Module-provided library tile and hero-art slots.
- Scrollable DLC/add-on lists whose scrollbar appears only when required.
- Working profile selection shared between the header and Profiles page.
- Profile creation, duplication, activation, editing, deletion, and per-profile folder browsing in front-end state.
- Global folder browsing for games, saves, modules, screenshots, and cache locations.
- Theme, accent-colour, and corner-style customisation with live application to the launcher.
- Capability-gated Settings sections so UI categories can be exposed/hidden from the front-end configuration until a backend service is ready.
- Host-aware About/Runtime information including current architecture, platform, Qt version, and available graphics backends.
- Persistent front-end preferences through `QSettings`.
- Custom frameless-window minimise, maximise/restore, and explicit close/exit controls.

The launcher does **not** download commercial game files or DLC. Users provide their own legally obtained local content and the appropriate game module is responsible for identifying and validating it.

## Test mode

Production builds start empty. For launcher development, use the CMake option:

```powershell
-DXENON_LAUNCHER_TEST_MODE=ON
```

Test mode injects fictional launcher-only data such as `Xenon Test Flight`, test modules, profiles, and enough fake add-ons to exercise scrolling and status states. No real game content is bundled or implied.

Example Windows UI-only configuration:

```powershell
cmake -S . -B build\launcher-ui -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_PREFIX_PATH="C:\Qt\6.10.3\msvc2022_64" `
  -DXENON_BUILD_LAUNCHER=ON `
  -DXENON_LAUNCHER_TEST_MODE=ON `
  -DXENON_BUILD_TESTS=OFF `
  -DXENON_ENABLE_MEMORY=OFF `
  -DXENON_ENABLE_GRAPHICS=OFF `
  -DXENON_ENABLE_VULKAN=OFF `
  -DXENON_ENABLE_D3D12=OFF `
  -DXENON_ENABLE_DXC=OFF
```

Then:

```powershell
cmake --build build\launcher-ui --target xenon_launcher -j 8
.\build\launcher-ui\launcher\xenon_launcher.exe
```

## Front-end capability switches

`launcher/src/launcher_config.hpp` contains the first capability table used by the QML UI. For example:

```cpp
struct UiFeatures {
  bool settings_graphics = true;
  bool settings_input = true;
  bool settings_audio = true;
  bool settings_network = false;
  // ...
};
```

This lets a section exist in source without forcing it into the shipping UI before its contract is ready. Later the runtime service registry can supplement these compile-time defaults with live capabilities.

## Module artwork/content contract

The generic Xenon launcher does not embed game artwork. Future module metadata should supply, where applicable:

- Library tile artwork.
- Selected-game hero/banner artwork.
- Game title and description.
- Supported game IDs/regions.
- DLC/add-on catalogue and installed state.
- Compatibility/runtime state.

Game-specific data flows from a game module into Xenon. Xenon must not depend on any individual game project.

## Backend integration seam

The production launcher now has a concrete core behind the QML facade:

```text
QML UI -> LauncherBridge -> FrontendBackend -> LauncherCore services -> RuntimeBridge -> Xenon::Core
```

Launcher behaviour is split into feature-owned C++ slices for application state, settings/paths, profiles, library/DLC, modules/catalog/settings, import/export, updates, filesystem integration, diagnostics, branding, runtime projection and launching. Production persistence is implemented by shared Launcher Core services, while test fixtures live in the same feature layer so QML does not maintain a second set of business rules.

The runtime boundary is intentionally truthful: the current `xenon::Runtime` API supports initialization/shutdown but does not yet expose a generic game-session execution API or live subsystem registry. `RuntimeBridge` therefore validates and carries a launch contract to that boundary without reporting a fake successful launch. When `Xenon::Filesystem` is built, the launcher now uses the framework content probe to identify extracted/root `default.xex` content and direct XEX2 files and matches their Xbox title/media metadata against installed module manifests. GDFX disc-image parsing is now available through `Xenon::Filesystem`, including `.dvd` descriptor resolution and XEX2 identity probing inside the image. STFS/DLC packages, module ABI loading, save services, and actual session execution remain later framework work below the launcher API.

See [`BACKEND.md`](BACKEND.md) and [`src/frontend_backend/README.md`](src/frontend_backend/README.md) for feature/service ownership, persistence, the launch contract, QML boundary rules, and the remaining Xenon framework seams.

## UI design system and engineering rules

The launcher uses a small design system rather than page-specific styling. The goal is to keep the interface consistent as more runtime services become available and to make the front-end credible as a standalone desktop application.

Current conventions:

- **Qt Quick Controls Basic style** is the styling base. Xenon customises one cross-platform style rather than attempting to restyle platform-native controls.
- **Semantic theme tokens** live in `Theme.qml`. Pages consume `surface`, `text`, `accent`, `success`, `warning`, `danger`, spacing, radius, and typography tokens instead of hard-coded colours and sizes.
- **System appearance is the default**. The `System` theme follows the host light/dark colour scheme. On Qt 6.10+, supported system contrast hints are also reflected in the launcher.
- **Typography follows a restrained desktop type ramp**: 12 px captions, 14 px normal UI text, 18/20 px section hierarchy, and 28 px page titles, with user-selectable text scaling.
- **Spacing uses a 4/8/12/16/24/32 scale**. This prevents each page from inventing its own padding and produces more predictable alignment.
- **Settings apply immediately**. There is no global Save button for ordinary preferences. Destructive operations such as deleting a profile still require confirmation.
- **Settings content is constrained to a readable width** and scrolls vertically on large pages rather than stretching controls across the full window.
- **Settings is pinned at the bottom of primary navigation** while Library, Modules, and Profiles remain the main destinations.
- **Navigation adapts at narrower window widths** to an icon-only compact rail so content receives priority without removing access to top-level destinations.
- **Pages are lazy-loaded once** when first visited. Expensive future backend work must remain asynchronous and must not block the QML/render thread.
- **Keyboard focus is explicit** on custom controls, navigation items, profile selection, menus, game tiles, and window controls. Interactive custom components expose `Accessible` metadata.
- **Status is never colour-only**. Labels, icons, or text accompany success/warning/error colours.
- **Game/module lists use lightweight delegates**. Rich detail UI is created only for the selected item.
- **Module-provided artwork is optional presentation data**, not a layout requirement. Empty and missing-artwork states remain usable.
- **Front-end state and backend logic remain separate**. QML owns presentation and immediate interaction; C++/future runtime services own discovery, validation, persistence, launch, and other substantive work.

When adding new UI, prefer extending the reusable `X*` components and semantic tokens instead of adding one-off raw `Rectangle`, `TextField`, `ComboBox`, or hard-coded colour implementations to individual pages.

## Windows launcher build helper

For day-to-day UI iteration, `launcher/scripts/build-launcher.ps1` wraps the repeated CMake/Qt deployment steps. From the repository root:

```powershell
.\launcher\scripts\build-launcher.ps1 -TestMode -Clean -Deploy -Run
```

If Qt is installed somewhere other than the common `C:\Qt\...\msvc2022_64` layout, pass it explicitly:

```powershell
.\launcher\scripts\build-launcher.ps1 `
  -QtRoot "C:\Qt\6.10.3\msvc2022_64" `
  -TestMode -Clean -Deploy -Run
```

Omit `-TestMode` for the normal empty-library production behaviour. The helper defaults to `RelWithDebInfo`; use `-Configuration Debug` only when a debug Qt build is specifically needed.


## Front-end regression testing

See [`TESTING.md`](TESTING.md) for the front-end regression checklist used before packaging launcher UI revisions.

## Live Input frontend and module API

The launcher Input page is now backed by the real `Xenon::Input` subsystem when
that target is compiled. It exposes live connected devices, stable identities,
four Xbox-user routes, optional multi-source assignment, profile selection,
background-input policy, rumble testing and support diagnostics. These values
are persisted through Launcher Core rather than QML-owned state and are carried
into `LaunchConfiguration` for the future game-session runtime.

Game modules do not consume the Qt frontend. A module may instead negotiate the
versioned native Input ABI with `runtimeApis.input` in its manifest and include
`xenon/input/module_api.hpp` through the header-only `Xenon::InputAPI` target.
For example Project Gracemeria can consume the runtime's final merged/profiled
Xbox-visible state without importing SDL/XInput or private `InputSystem` types.
See `../docs/modules/INPUT_API_V1.md`.

The current runtime still lacks the final generic game-session/native-module
loader, so the launch contract records the API requirement and input routing
without pretending a module has already been handed a live function table. The
future session loader only needs to create the runtime-owned Input API provider
and pass its `ApiV1` table to the negotiated module; the Input ABI itself is
already defined.
