# Xenon Launcher Next

Separate CEF + React/TypeScript launcher conversion. The retired Qt launcher is kept locally under ignored `../launcher/`; it is no longer versioned. Xenon runtime builds remain separate. **The Windows desktop package runs; full legacy feature parity is still in progress.** See [MIGRATION_STATUS.md](MIGRATION_STATUS.md) for verified work and remaining gaps.

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

- CEF Views desktop window, sandbox bootstrap on Windows, packaged local assets, origin-restricted asynchronous command bridge, bounded native worker queue and renderer error logging.
- Original System/Xenon Dark/Carbon/Industrial/Light theme catalogue, palettes, all seven raster backgrounds, decorative SVGs and branding. Accent presets/custom colour, corners, per-theme backgrounds, intensity, decorations, panel transparency, high contrast, text scaling, reduced motion and system appearance changes.
- Library source registration, search/filter/sort, virtualized list/grid and optional carousel, favourites, metadata/module association, local module cover/hero artwork, local DLC references and source-path checks. Registration does not assert game compatibility or integrity.
- Persistent profiles, edits, duplication, activation/removal guards, legacy-compatible portable profile import/export, native avatar import/removal and non-destructive focal-point/zoom metadata.
- Extracted module registration, schema-driven settings, enable/disable, manifest checks, validated ZIP staging/installation, previous-version retention and rollback. No module binary is executed by the launcher.
- Home/history and notification surfaces, global navigation/search, keyboard shortcuts and foreground controller focus navigation.
- Capture listing/viewing/deletion through bounded resource IDs. Local support-summary ZIPs, clipboard/export diagnostics, folder opening, recovery markers and safe-mode restart.
- Visible pre-React loading/error screens, a React render error boundary and a native main-document failure page. Failed startup does not silently reset state.

Some surfaces remain partial: online catalogue/download/update providers, background job cancellation, full log sanitization, legacy bulk state migration, richer capture browsing, input-provider routing and production runtime integration. A converted page is not evidence of parity.

## Native builds and validation

```powershell
npm run build
npm test
npm run desktop:build
```

Native core/service/archive/runtime-adapter tests:

```sh
cmake -S . -B build/core -DCMAKE_BUILD_TYPE=Release
cmake --build build/core --config Release
ctest --test-dir build/core -C Release --output-on-failure
```

Desktop end-to-end tests connect to an explicitly enabled debug endpoint. Use an isolated data directory, not your real launcher data:

```powershell
.\build\native\desktop\xenon_launcher_next.exe --remote-debugging-port=9223 --data-dir="C:\path\to\launcher-next\build\smoke-data"
npm run test:desktop
```

The smoke test creates fictional test-only profile/module/content references in that isolated state, exercises native IPC and appearance, forces a script-load failure, checks recovery, takes screenshots and closes the test window. Remote debugging is not enabled by normal launches.

An explicit `--fixture-host="C:\path\to\launcher-next\build\native\xenon_launcher_fixture_host.exe"` argument enables the separate test-process adapter. With registered test content, `node scripts/smoke-session.mjs` verifies the launch handshake, asynchronous stop and history. The fixture runs no game or Xenon code. Normal launches do not discover or enable it. `node scripts/smoke-restart.mjs` checks a safe-mode restart against an isolated debug instance.

Linux core builds use the same CMake project. A CEF desktop build additionally needs the matching SDK (`npm run cef:fetch`), platform dependencies and `-DXENON_NEXT_BUILD_DESKTOP=ON -DCEF_ROOT=...`. Linux desktop code is not yet live-validated here. macOS core/platform seams exist, but the required helper application bundles remain an explicit desktop-build blocker; do not advertise macOS desktop support yet.

## Source ownership

`web/ui/` owns presentation and immediate interaction. `contracts/` owns the wire model and converted catalogues. `native/application/` owns authoritative state/workflows. `native/core/` owns domain/archive primitives; `native/platform/` owns OS operations; `native/host/` owns CEF.

Repeatable legacy catalogue/asset conversion:

```sh
node scripts/convert-appearance.mjs
node scripts/convert-settings.mjs
```

The converters read the original launcher and write only this folder. C++ services do not depend on Qt. Runtime-dependent controls remain unavailable until a real adapter supplies them. No game content or fictional production library is bundled.

The original migration plan and legacy inventory remain in the local, ignored `../launcher/` folder. Conversion scripts also require that local source tree. Full parity, platform, migration and packaging gates remain open.
