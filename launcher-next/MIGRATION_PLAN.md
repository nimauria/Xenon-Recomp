# Launcher migration: Qt/QML to CEF + React

Status: proposed implementation plan, 28 October 2026. This document does not claim the migration has been implemented or validated.

## Outcome and scope

Build a fully functional standalone Xenon launcher with CEF, React, TypeScript, CSS and Vite. Remove Qt and QML from the final launcher, including backend Qt dependencies, build helpers, deployment and tests. Keep native launcher services in C++. Windows and Linux are release targets; preserve a viable macOS architecture and establish an early macOS build smoke test rather than discovering platform assumptions at the end.

This milestone owns launcher presentation, local persistence, imports, module management, settings, profiles, captures, operations, desktop integration and diagnostics. It does not implement new Xenon runtime features. Existing runtime contracts are compatibility inputs; do not modify runtime or game-module interfaces to accommodate presentation.

Standalone means actual persistence and native operations, not a collection of clickable mock pages. Runtime-dependent actions expose unavailable capabilities in production. Development uses explicit fixture adapters and a separate test host. Finishing the launcher makes later Xenon work an adapter integration task, but does not prove that integration works: content identification, capability negotiation, preparation, input and session lifecycle still need live validation later.

## Findings from the current tree

- 55 top-level QML files, plus nested page components. The migration inventory lists every QML source and existing bridge declaration.
- Presentation is separated into `LauncherBridge`, feature-owned `FrontendBackend`, core services and runtime/content adapters. Preserve feature ownership, not the Qt implementations.
- `LauncherCore`, services and `FrontendBackend` use QObject; models and runtime interfaces expose QString/QVariant. QSettings, QSaveFile, QProcess, QTimer, Qt networking and platform helpers also require replacement.
- Current behaviour includes Home, Library/DLC/properties, Modules/catalog/settings, Profiles/avatar editing, Settings, Captures, Downloads, Notifications, Support, input, network, filesystem and developer surfaces.
- Existing launch configuration and process supervision are documented in `BACKEND.md`, `src/runtime/runtime_bridge.*` and `../docs/runtime/RUNTIME_HOST.md`.
- Existing testing and design documents are parity inputs. Source presence is not evidence that every current function works; establish baseline behaviour before marking a feature migrated.

## Product layout

Use a Steam-inspired desktop layout with Xenon's own branding and styling: persistent application shell, compact navigation, searchable library list/grid, selected-game artwork and details, prominent Play action, and persistent activity/session status. Preserve selection, filters and scroll position when navigating.

Primary destinations: Library, Home/activity, Modules and Profiles. Settings is anchored at the bottom. Captures and Downloads have direct destinations or persistent shortcuts. Support/About remain easy to find. Runtime filesystem/network/debug tools live in Developer rather than crowding normal navigation. Existing functionality must have a documented destination even if its page changes.

Use one shared design system: semantic CSS tokens, accessible primitives, predictable focus, reusable dialogs, vector icons, light/dark/high-contrast appearance, reduced motion and 100–200% text scaling. Window controls must support native resizing, dragging, maximize, multi-monitor/DPI restoration and OS shortcuts. Start with reliable native window decoration; custom chrome follows platform validation.

## Architecture

```text
React application (renderer; presentation and ephemeral interaction state)
        |
        | versioned asynchronous commands, results and events
        v
CEF host (trusted origin, transport, window/platform integration)
        |
        v
Launcher application services (Qt-free C++; authoritative state/workflows)
        |
        +-- local persistence, files, jobs, modules, profiles, settings
        +-- IContentProbe / IRuntimeAdapter / IInputProvider
                      |
                      +-- explicit development fixtures and test host now
                      +-- Xenon adapters at the integration milestone
```

CEF browser, renderer and GPU subprocesses are implementation processes, separate from the game runtime host. Keep scans, hashing, archive operations and network processing on workers. Return results/events to the correct CEF thread. Do not add a local HTTP server or ship Node.js merely to serve the interface. Vite produces packaged static assets loaded from a registered application origin.

Use the supported CEF native window/Views path first. Validate Linux display integration and accessibility against the pinned distribution. Avoid custom off-screen rendering/compositing unless a demonstrated requirement justifies the extra input, accessibility and GPU work.

Proposed source ownership:

```text
launcher/web/                 React app, routes, components, CSS, typed client
launcher/contracts/           versioned schemas, fixtures and API documentation
launcher/native/host/         CEF lifecycle, resource handling and IPC
launcher/native/application/  feature workflows and projections
launcher/native/core/         Qt-free domain models and services
launcher/native/platform/     Windows/Linux/macOS implementations
launcher/native/adapters/     content, runtime and input provider seams
launcher/tests/               core, contract, fixture-host and desktop tests
```

These are target directories; migrate incrementally rather than moving all current files before behaviour is covered.

## Contract before pages

Define commands and DTOs by feature, not a generic callable mirror of every Q_INVOKABLE. Use stable game/module/profile IDs rather than list indices. Validate JSON at the native boundary; TypeScript types alone are insufficient. Select one schema source and generate or verify both C++ and TypeScript bindings.

Each request has protocol version, request ID, command and payload. Results return either typed data or a structured error with code, user message and optional recovery action. Async work returns a job ID; events carry entity ID, revision and progress. Specify cancellation, duplicate requests, timeouts and terminal job states. A lost renderer subscription must be recoverable through a fresh snapshot and revision reconciliation.

Required command families: application/window/recovery; appearance/settings/paths; profiles/avatars/import/export; library/properties/DLC/import/verification; modules/catalog/schema/settings/install/update/rollback; jobs/downloads; captures; search/notifications/home/history; diagnostics/support/community; input; runtime/session; developer filesystem/network.

Keep browser-only toasts, focus, selection and open dialogs in React. Native services own validation, persistence, file mutation, runtime state and long-running operations. Commands must distinguish accepted work from completed work. A spawned process does not establish a Running game session.

Constrain the bridge to the packaged top-level origin/frame. No arbitrary filesystem/shell execution API, no native bridge in external websites, and no remote scripts in privileged views. Open external links in the system browser. Serve artwork/captures through bounded opaque resource IDs; reject traversal and avoid exposing arbitrary file URLs. Retain CEF sandbox support and validate its packaged configuration per platform.

## Page and function translation

| Current source/surface | New destination and behaviour to preserve |
|---|---|
| Main, Sidebar, TopBar, FooterBar, QuickCenter | Application shell, routing, remembered page, profile switch, window/session controls, quick actions |
| HomePage | Real recent activity, session history and resumable actions; honest empty state |
| LibraryPage, GameTile, CoverImage, ArtworkFrame | Virtualized grid/list, stable selection, favourites, search/filter/sort, artwork fallback/cache, import and managed-content actions |
| GamePropertiesDialog, library feature/DLC | Game details, compatibility, paths, verification, DLC add/remove/verify, module settings and launch summary |
| ModulesPage, ModuleCatalogDialog, ModuleSettingsDialog | Discovery, import, enable/disable, verification, uninstall/unlink, catalog, install/update/cancel/rollback and schema-generated settings |
| ProfilesPage, ProfileStore, ProfileMenu, ProfileEditorDialog | Create/edit/duplicate/activate/delete, immutable IDs, import/export, default/path overrides and dirty-form protection |
| ProfileAvatar, AvatarCropEditor | Local import, bounded image decoding, original-preserving crop/focal point/zoom, remove avatar |
| SettingsPage and nested settings pages | Every existing category/key/default/validation/reset, theme/accent/backgrounds, global paths, accessibility and capability gates |
| CapturesPage | Real folder listing, virtualized thumbnails, viewer/zoom, refresh and confirmed permanent deletion |
| DownloadsPage | Durable operation status, bytes/progress/error, cancel/retry and supported install/update actions; do not imply unsupported pause/resume |
| CommandPalette, HelpPopover | Global ranked search/actions, keyboard shortcuts and contextual help |
| NotificationCenter, Toast | Durable history/read/dismiss/action routing plus transient feedback; avoid duplicate events |
| RecoveryDialog, NavigationGuard | Crash recovery, safe mode/restart, dirty navigation confirmation and readiness marker |
| SupportPage, DeveloperPage | Diagnostics, copied summaries, privacy-filtered local support bundle, logs, build/platform/backend information |
| FilesystemPage, NetworkPage | Preserve command/state contracts; production shows unavailable runtime tools until an adapter provides them |
| Theme, ThemeBackdrop, XenonBrand, XIcon, StatusPill, EmptyState, X* controls | Shared CSS tokens, SVG branding/icons, accessible React primitives and consistent loading/empty/error states |
| Input settings and frontend actions | Device discovery/routes/profiles where a provider exists; foreground-only controller UI navigation, focus graph and glyph selection |
| Community/update/system services | Community links, optional presence, single instance, navigation-only deep links, window state and platform integration |

Every inventory entry must be marked migrated, merged into a named replacement, or explicitly deferred with a reason and visible unavailable behaviour. No silent page/function removal. The implementation inventory must include nested controls, bridge properties/signals, service APIs, settings keys, persistence schemas and feature gates, not only visible pages.

## Qt removal and persistence

Replace Qt strings/variants with typed UTF-8 domain models, standard containers and an explicit JSON boundary. Use std::filesystem with native Unicode conversion at OS boundaries. Replace QObject signals with application subscriptions whose lifetimes are explicit; replace timers/processes/networking with platform-neutral interfaces and platform implementations. Audit existing repository libraries before selecting new dependencies.

Use versioned JSON for modest launcher state initially, with atomic replacement, schema validation, backups and explicit migrations. Keep larger binary assets separate. Do not add a database merely to reproduce small existing documents. Native services remain the single writer; preserve IDs, paths and settings semantics.

QSettings is a migration task, not just a file rename: audit Windows registry storage, Linux configuration files and macOS preferences using the old application/organization identifiers. Implement a read-only legacy importer without a shipping Qt dependency. Back up source data, stage conversion, validate, commit atomically and record migration version. Keep old state intact on failure. Preserve profile JSON/avatar references, library/module state, history, notifications and configured external storage roots. Decide portable-mode behaviour explicitly. Test Unicode, case-sensitive paths, symlinks, permissions and interrupted writes.

## COGBox reference review

Source reviewed: public repository README on 28 October 2026, https://github.com/Shagg88/COGBox . Source-directory fetching was unavailable during this review, so the following reflects documented features, not audited implementation or tested quality. COGBox documents SDL3/Dear ImGui, Windows support and experimental Proton use; its portability strategy is not our native Linux/macOS design.

| Documented idea | Xenon decision for this migration |
|---|---|
| Drag/drop local game copies and packages | Adopt a unified import entry point with classification, compatibility summary, destination choice, progress and actionable failure; support only formats the content adapter actually implements |
| Explanations on per-game settings | Preserve schema-driven module settings; add inline help, defaults, restart requirements and unsupported-option explanations |
| Plain-language crash reports and support ZIP | Preserve existing support-bundle service behaviour and improve the failure-to-report flow; local preview/export, no automatic upload |
| Controller navigation and platform button names | Preserve controller usability; establish focus/modal/back behaviour and Xbox/PlayStation glyph selection |
| Play time and library artwork | Preserve history/artwork slots; accurate session time requires real runtime status later. External artwork downloading is deferred |
| Save backups | Record as future capability; backup/restore semantics and active-session safety are outside migration parity |
| Per-game shortcuts | Retain a desktop integration seam; implement after core shell parity if scope allows, otherwise explicitly defer |
| Multiple languages | Externalize strings and use locale-safe formatting now; translation catalogue and language coverage are a later content task |
| DLC, mods and title updates | Preserve existing DLC operations. Add future provider seams where needed; new mod/load-order/title-update workflows are deferred |
| Self updates | Preserve check/download/install/cancel and module rollback contracts; application update installation must be validated independently per OS |

No COGBox feature is automatically evidence of a Xenon gap. Compare with the existing inventory before adding work. Do not expand into game-specific graphics settings, achievements, online services, in-game overlays or renderer functionality during launcher migration.

## Implementation sequence and exit gates

### 0. Freeze the behavioural baseline

Inventory every source, bridge entry, setting and persisted schema. Mark current behaviour verified, unavailable or unverified. Capture representative screens and keyboard/controller flows using empty and fictional fixture data. Preserve all unrelated working-tree changes. Exit: each existing function has a destination and acceptance case.

### 1. Prove CEF packaging and desktop foundations

Pin an exact supported CEF distribution/checksum per initial architecture and an explicit Node/package-manager toolchain for builds. Package assets locally; boot one React page with working native dialogs, window lifecycle, async command/event round-trip and renderer-crash handling. Build Windows and Linux packages; establish macOS helper/bundle build feasibility. Exit: packages launch on clean machines without Qt, system Chrome, a Vite server or runtime subsystem dependencies. Use native OS builds; track x64 and ARM64 availability explicitly.

### 2. Contracts and Qt-free services

Implement protocol validation, feature capabilities, job management, persistence and platform service interfaces. Port profile/settings/path/recovery services first. Add legacy migration fixtures. Establish IContentProbe, IRuntimeAdapter and IInputProvider with unavailable production implementations and explicit test implementations. Exit: native services work without React/CEF, and the same contracts work with fixture and production providers.

### 3. Design system and application shell

Build shared controls, tokens, routes, global search, notifications, profile switcher and activity/session surfaces. Add focus management, keyboard/controller actions, responsive layouts and locale-ready strings. Exit: navigation/window/accessibility flows work in the packaged desktop host with real persisted preferences.

### 4. Library and complete launch vertical slice

Implement library list/detail, local import workflow, artwork, properties, favourites, DLC and module association. Use a separate test runtime process to exercise validation, preparing, starting, running, stopping, stopped, failed and crash states. Include cancellation, duplicate Play, host timeout, renderer reload and launcher restart/reattach. Exit: real local state survives relaunch; test host drives the actual supervision path; production reports missing runtime capability honestly.

### 5. Remaining functional parity

Port Modules/catalog/settings/update/rollback, Profiles/avatar/import/export, Settings categories, Home, Downloads, Captures, Support/recovery and developer/input/network/filesystem contracts. Installers/updaters use isolated test packages/providers. Exit: every inventory entry is accounted for and parity acceptance cases pass; unavailable integrations are identified explicitly.

### 6. Performance and cross-platform hardening

Run Release builds against representative hardware and synthetic libraries of 0, 100, 1,000 and 10,000 entries. Measure cold/warm startup, input latency, frame times, memory, artwork cache and scanning responsiveness. Initial targets: p95 local interaction feedback within 100 ms and smooth 60 Hz scrolling on documented reference hardware, with native work never blocking rendering. Establish measured memory/startup budgets after the CEF baseline; do not promise a framework swap is automatically faster.

Exercise keyboard, controller reconnect, screen readers, 200% scaling, reduced motion, mixed DPI, missing artwork, offline mode, proxy/TLS errors, disk full, permission failures and case-sensitive paths. Check Linux X11/Wayland behaviour and packaged sandbox dependencies. macOS needs helper bundles, permissions, application lifecycle, signing/notarization planning and architecture coverage even if public release comes later. Exit: Windows/Linux clean-machine packages and macOS feasibility evidence, with remaining platform blockers stated plainly.

### 7. Cut over and remove Qt

Make the new launcher the default only after parity gates pass. Temporary coexistence of old and new build targets is a migration aid, not the final architecture. Remove QML, Qt bridge/backend code, moc/rcc resources, Qt package discovery/linking, deployment tooling, Qt-only tests and outdated documentation. Convert reusable assets/tests rather than deleting coverage. Update root build/package/CI wiring and launcher documentation. Exit: source/build dependency audit and package dependency inspection show no Qt/QML, including transitive launcher dependencies; state migration succeeds on copied legacy data.

### Later: connect Xenon

Implement and validate the real adapters against the frozen contracts: content probe, capabilities, preparation/module requirements, launch configuration, host status/logs/stop/reconnect, runtime input and developer tools. Retain existing file/process contracts where compatible. Exit: real game launch, errors, stop and recovery are tested. A fixture-host success is launcher readiness, not evidence of game/runtime correctness.

## Validation and definition of done

Use focused C++ tests for persistence, migration, paths, validation, job/session state and provider boundaries. Use frontend tests for meaningful state/focus behaviour and contract failures. Use browser automation for workflows plus packaged CEF smoke tests: a normal browser test cannot validate native dialogs, CEF IPC or desktop lifecycle. Reuse current regression cases and test-host patterns where applicable; do not copy Qt-specific test plumbing into the new architecture.

The launcher milestone is complete when all migration inventory entries are resolved; native local operations and persistence work; the fixture host proves complete session lifecycle; Windows/Linux packages operate without Qt; macOS has no unexplored architectural blocker; unsupported runtime operations are honest; accessibility and measured performance gates pass; and build/release/user documentation describes the new launcher accurately. No claim of full Xenon integration is made before live adapter validation.

## References

- Existing launcher: `README.md`, `BACKEND.md`, `DESIGN.md`, `TESTING.md`, `src/launcher_bridge.hpp`, `src/frontend_backend/`, `src/services/`, `src/runtime/`.
- COGBox documented product behaviour: https://github.com/Shagg88/COGBox
- CEF architecture, distributions, subprocesses, threading, IPC and platform layout: https://chromiumembedded.github.io/cef/general_usage.html
- Astro islands reference considered during technology selection: https://docs.astro.build/en/concepts/islands/ . Astro is not included in the selected launcher stack.
