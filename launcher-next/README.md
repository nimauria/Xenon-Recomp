# Xenon Launcher Next

Separate CEF + React/TypeScript launcher conversion. The retired Qt launcher is kept locally under ignored `../launcher/`; it is no longer versioned. Xenon runtime builds remain separate. **Windows and Linux native builds pass; remaining acceptance gates are recorded separately from feature implementation.** See [MIGRATION_STATUS.md](MIGRATION_STATUS.md) for verified work and remaining gaps.

The [design direction](DESIGN_DIRECTION.md) combines Steam's page structure, GOG GALAXY's collection browsing and COGBox's focused game view with Xenon's existing themes. Navigation, shared controls and library presentation are being established during conversion; larger new product features follow parity.

## Run on Windows

Requires Node.js 24 LTS, Visual Studio C++ build tools, the Windows SDK and CMake/Ninja. Node is a build/development dependency, not a packaged runtime dependency.

```powershell
cd launcher-next
npm ci
npm run cef:fetch
npm run desktop
```

`cef:fetch` verifies the pinned vendor archive checksum and reuses an existing download. The CEF SDK stays under ignored `.tools/`. `desktop` builds the web assets and native host and launches `build/native/desktop/xenon_launcher_next.exe`. Close the desktop launcher before rebuilding its DLL.

For browser-only interface work:

```powershell
npm run dev
```

Browser preview uses its own localStorage. Native imports, package installation, filesystem mutation and runtime launching are unavailable there. The native app stores its state separately under `%LOCALAPPDATA%/Project Xenon/Launcher Next`. Existing Qt launcher data is never overwritten automatically.

## Implemented conversion slices

- English master catalogue and French/German/Spanish UI translations, persistent language selection, translated command search and safe English fallback. See [LOCALIZATION.md](LOCALIZATION.md) for authoring and remaining native-message work.
- CEF Views desktop window, sandbox bootstrap on Windows, packaged local assets, origin-restricted asynchronous command bridge, bounded native worker queue and renderer error logging.
- Original System/Xenon Dark/Carbon/Industrial/Light theme catalogue, palettes, all seven raster backgrounds, decorative SVGs and branding. Accent presets/custom colour, corners, per-theme backgrounds, intensity, decorations, panel transparency, high contrast, text scaling, reduced motion and system appearance changes.
- Library source registration, search/filter/sort, virtualized list/grid and optional carousel, favourites, metadata/module association, local module cover/hero artwork, local DLC references and source-path checks. Registration does not assert game compatibility or integrity.
- Persistent profiles, edits, duplication, activation/removal guards, legacy-compatible portable profile import/export, native avatar import/removal and non-destructive focal-point/zoom metadata, nine isolated runtime overrides, original profile limits/regions and startup-page choices.
- Extracted module registration, schema-driven settings, enable/disable, manifest checks, validated ZIP staging/installation, previous-version retention and rollback. No module binary is executed by the launcher.
- Home collection counts, favourite/recently-added shelves, direct game navigation and labelled test-session history; page-specific search, preserved library browsing state, keyboard shortcuts and foreground controller focus navigation.
- Capture listing/viewing/deletion through bounded resource IDs. Local support-summary ZIPs, clipboard/export diagnostics, folder opening, recovery markers and safe-mode restart.
- Background module ZIP jobs with progress, cancellation before commit, retry, interrupted-work recovery and persistent shell status. A renderer reload leaves the native job running. See [JOBS.md](JOBS.md).
- Visible pre-React loading/error screens, a React render error boundary and a native main-document failure page. Failed startup does not silently reset state.

The original registry/catalogue schema, cached/bundled entries, native HTTPS, verified release downloads, module updates/install/rollback and update history are connected. Captures use an asynchronous bounded index and virtualized thumbnails with zoom/navigation. Copied legacy data can be reviewed and imported under Settings → General, with backups and unchanged originals.

The production session bridge can now use the real Xenon runtime host. Settings → Runtime selects a host; an explicit `--runtime-host` or a host beside the launcher is also supported. See [INTEGRATION.md](INTEGRATION.md) for running, verified evidence and the remaining preparation/content/live-service work. Native-message translation, Mac and broader accessibility/distribution checks remain open. Optional Discord presence has an opt-in local IPC provider.

## Planned graphics controls

The [graphics menu plan](../docs/graphics/LAUNCHER_GRAPHICS_MENU_PLAN.md)
describes global/per-game reconstruction, frame-generation controls and
reporting of the active renderer state. It accompanies the
[shared XeSS/FSR/DLSS plan](../docs/graphics/UPSCALING_FRAME_GENERATION_PLAN.md)
and [per-game rendering support plan](../docs/graphics/UNIVERSAL_RENDERING_SUPPORT_PLAN.md).
These controls and SDK integrations are planned, not implemented launcher
features. The existing runtime-host bridge does not establish enhancement
support or title playability.

## Native builds and validation

```powershell
npm run build
npm test
npm run desktop:build
```

Native core/service/archive/runtime-adapter and migration tests:

```sh
cmake -S . -B build/core -DCMAKE_BUILD_TYPE=Release
cmake --build build/core --config Release
ctest --test-dir build/core -C Release --output-on-failure
```

Desktop end-to-end tests connect to an explicitly enabled debug endpoint. Use an isolated data directory, not your real launcher data:

```powershell
.\build\native\desktop\xenon_launcher_next.exe --offline --remote-debugging-port=9223 --data-dir="C:\path\to\launcher-next\build\smoke-data"
npm run test:desktop
```

The smoke test creates fictional test-only profile/module/content references in that isolated state, exercises native IPC and appearance, forces a script-load failure, checks recovery, takes screenshots and closes the test window. Remote debugging is not enabled by normal launches.

For background installation/cancellation/retry tests, start another isolated debug instance and run `npm run test:jobs`. It creates test-only module packages, checks a renderer reload during installation, clears its completed job records and closes the window.

An explicit `--fixture-host="C:\path\to\launcher-next\build\native\xenon_launcher_fixture_host.exe"` argument enables the separate test-process adapter. With registered test content, `node scripts/smoke-session.mjs` verifies the launch handshake, asynchronous stop and history. The fixture runs no game or Xenon code. Normal launches do not discover or enable it. `node scripts/smoke-restart.mjs` checks a safe-mode restart against an isolated debug instance.

Linux core builds use the same CMake project. A CEF desktop build additionally needs the matching SDK (`npm run cef:fetch`), platform dependencies and `-DXENON_NEXT_BUILD_DESKTOP=ON -DCEF_ROOT=...`. Linux has passed sandbox-enabled Xvfb startup and the four-language smoke checks in the isolated Ubuntu validation container. macOS now has main/helper bundles, dynamic framework loading, native preference discovery and a CI build lane; its first real build/runtime result remains unverified.

## Source ownership

`web/ui/` owns presentation and immediate interaction. `contracts/` owns the wire model and converted catalogues. `native/application/` owns authoritative state/workflows. `native/core/` owns domain/archive primitives; `native/platform/` owns OS operations; `native/host/` owns CEF.

Repeatable legacy catalogue/asset conversion:

```sh
node scripts/convert-appearance.mjs
node scripts/convert-settings.mjs
node scripts/convert-profile-runtime.mjs
```

The converters read the original launcher and write only this folder. C++ services do not depend on Qt. Runtime-dependent controls remain unavailable until a real adapter supplies them. No game content or fictional production library is bundled.

Re-running those legacy converters requires the locally retained `../launcher` sources; normal builds use the converted contracts and assets already in this project.

The completed migration inventory is summarised in [MIGRATION_STATUS.md](MIGRATION_STATUS.md), alongside remaining platform, migration and packaging acceptance. Legacy conversion scripts still require the local source tree; normal builds do not.

Additional isolated desktop checks: `node scripts/smoke-profiles.mjs` (requires the explicit runtime fixture), `node scripts/smoke-captures.mjs`, `node scripts/smoke-migration.mjs`, and `node scripts/smoke-crash.mjs`. Test windows are closed when each check finishes; they are separate from the normal launcher. For network checks, generate `node scripts/create-network-fixtures.mjs`, start with `--fixture-network="<absolute build/network-fixtures>"` and run `npm run test:network`.

## Distribution and self-updating

`npm run desktop:package` creates a fresh Windows x64 payload, portable ZIP and SHA-256 sidecar under `build/packages/`. The package contains CEF, web assets, licences, immutable catalogues and a separate updater; it does not ship Node, Qt or user data. The default build version is 0.1.0; CMake `XENON_NEXT_VERSION` supplies the compiled distribution version.

Automatic replacement is offered only by a marked packaged Windows installation, with no active game or file operations. It rechecks the downloaded digest and package product/platform/version/state schema, extracts beside the installation and verifies staged files again. The separate installed helper waits for the exact parent identity, retains the old folder, starts the replacement and waits for an interface-ready receipt. Failed startup restores the previous folder. Backups and failed payloads are retained under the sibling `.xenon-updates` folder; check `update-install.log` for diagnosis. Development builds and other platforms use manual replacement. Never point an update test at a normal installation.

`create-updater-fixture.mjs <old-payload> <new-package.zip>` and `smoke-updater.mjs` exercise isolated real package replacement. Additional checks are `test:library`, `test:services`, `test:shell`, `scripts/smoke-parity-actions.mjs`, `scripts/smoke-dlc-catalog.mjs` and `scripts/smoke-startup-failure.mjs <isolated-fixture-data>`. Chooser selection remains a manual gate; automated content fixtures use native RPC.

See the [current migration evidence and gates](MIGRATION_STATUS.md). The original 183 invokable methods and 55 QML components were accounted for; platform/performance/accessibility and live-provider acceptance remain open.

`npm run test:media` and `npm run test:modules` require fresh isolated desktop instances with the explicit fixture host and debug endpoint described above. They exercise grouped discs/primary launch sources, metadata/unsaved changes, separate test activity, API declaration checks and owned-module uninstall. Their source paths and deletion requests use native RPC; the operating-system picker remains a separate acceptance gate. See [JOBS.md](JOBS.md) for ownership, cancellation and interrupted-deletion behaviour, and [MIGRATION_STATUS.md](MIGRATION_STATUS.md) for recorded suite results and compatibility boundaries.

`npm run test:presentation-paths -- build/presentation-paths-network` additionally requires a local network fixture generated with `node scripts/create-network-fixtures.mjs build/presentation-paths-network` and the desktop arguments `--fixture-network="<absolute fixture folder>" --fixture-host="<absolute fixture host>"`. It checks catalogue text/artwork/DLC refresh, unsaved-edit protection, storage defaults/reset, decoded captures, build information and launch paths. Use a new isolated data directory; omit `--offline` so the explicit network fixture is selected. No real network service is used.

Settings > Paths shows working destinations separately from fixed launcher storage. Game/save/module/capture destination changes affect future imports/launches and keep existing files at their recorded locations. Cache changes queue a verified copy of managed artwork/downloads and update package references atomically; previous copies are retained and listed by their managed subfolders. Browser cache stays with launcher data. Profile/settings data is stored together in `state.json`; unused old profile-folder values and cache values not yet applied remain visible for review. A different launcher data root can be supplied with `--data-dir` at startup.

`npm run test:profile-cache -- build/profile-cache-network` uses a fresh isolated desktop instance with `--fixture-network="<absolute fixture folder>"` and the debug endpoint. Generate that fixture first with `node scripts/create-network-fixtures.mjs build/profile-cache-network`; omit `--offline`. It tests Unicode profile creation/copy/import and inline duplicate errors, cache copy/reset/reload/image decoding, installation from copied packages, future downloads and unsupported-provider controls. Cache destination/source selection uses native RPC; no real network service is contacted.

Profile names use pinned Unicode 17.0 data in both native and preview validation. The mapping is locale-independent scalar lowercase with Unicode whitespace trimming, preserving display names and the 48 UTF-16-unit limit; it does not add case folding or Unicode normalization. Metadata edits to older Unicode-equivalent display names remain possible without forced renaming, while new duplicates are rejected. `node scripts/generate-profile-unicode.mjs` reproduces both tables using a Node build with Unicode 17.0. Normal builds use the checked-in data and do not regenerate it. The Unicode licence ships with the package. The baseline is the original locale-independent [QString lowercase policy](https://doc.qt.io/qt-6/qstring.html#toLower); the new launcher has no Qt dependency.

Legacy feature names map through `web/features.ts` to explicit capability gates. Runtime network status, runtime network configuration and the launcher's own update transport have separate availability. Unknown names return false; the [status record](MIGRATION_STATUS.md#compatibility-boundaries-retained-from-migration) summarises the preserved boundaries.

The [performance record](PERFORMANCE.md) includes process-startup samples and isolated 0/100/1,000/10,000-entry library measurements, runners and limits. Platform, total-memory and physical-input acceptance remain open.

## Populated developer demo

After building the desktop, run `npm run demo` for a persistent sample library, or `npm run demo:fresh` for a new isolated copy. Four fictional games, two profiles, original Xenon artwork, captures, notifications and sample jobs make the pages reviewable. Local verified packages exercise download, install and update actions; the explicit test host exercises sessions without running Xenon or a game. A visible banner identifies the demo.

Demo files stay below `build/demo/`; normal launcher data is separate. No sample library is shipped or automatically added to a normal launch. Use `npm run demo -- --prepare-only` to generate the samples without opening a window.

Updates is a dedicated page for launcher/module releases. Download and Install controls remain visible with explanations when unavailable. Downloads also lists verified packages that are ready to install, including packages whose old job records were cleared.

Discover exposes catalogue search and platform/installed/downloaded filters as a normal page. Home's Needs attention shortcuts lead to ready packages, updates, failed jobs and missing sources. See [current evidence](MIGRATION_STATUS.md) and [planned product follow-ups](DESIGN_DIRECTION.md#planned-product-follow-ups).

## Per-game settings and repeatable acceptance

Every selected game has **Game settings**, also available in its action menu. Graphics, Audio, Controls and Game Options are saved for that game only. Unchecked overrides inherit active-profile/launcher values; game options inherit module defaults. Saving is atomic, changes apply on the next launch, and settings for the active game are locked. The separate **Module defaults** control intentionally changes shared defaults.

`npm run test:acceptance` runs isolated CEF workflows with fresh data, local fixtures and owned processes. Pass scenario names to narrow a run, for example `npm run test:acceptance -- game-settings controller localization profile-cache`. Reports live under ignored `build/desktop-validation-*`. `XENON_TEST_DESKTOP` selects an already packaged payload; `XENON_TEST_BUILD` selects the native fixture build. Tests never opt into real Discord publication.

`npm run desktop:build` and `npm run desktop:package -- --skip-build` select the host platform. Windows produces a ZIP; Linux/macOS produce a tar.gz with a SHA-256 sidecar. macOS packages still require real build, signing/notarization and runtime acceptance before distribution. Linux distributions need compatible CEF system libraries and a supported sandbox configuration. No Node, Qt or development server is shipped.
