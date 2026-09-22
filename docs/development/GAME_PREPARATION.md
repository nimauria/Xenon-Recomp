# Automatic Game Preparation (Disc Import → First-Play Build → Cache)

This document describes Xenon's end-to-end consumer workflow for turning a
legally-owned Xbox 360 disc image into a running game, with no manual
`default.xex` extraction and no manual invocation of the recompiler. It
covers: disc mounting, the managed game library, the artifact cache, the
`xenon-prepare` worker, and the launcher-side wiring. It supersedes any
assumption that Xenon requires a loose `default.xex` as the only supported
input.

## 1. Disc image mounting (already-existing foundation)

Xbox 360 disc images (`.iso`/`.xgd`/`.dvd`) are read directly, with no
extraction, via:

- `xenon::filesystem::GdfxImageSource` (`include/xenon/filesystem/gdfx_image_source.hpp`,
  `src/filesystem/content/gdfx_image_source.cpp`) — a real GDFX/XDVDFS
  parser: locates the game partition (scanning the layouts real Xbox 360 disc
  dumps use), parses the binary directory tree, and serves `stat`/`list`/
  `read_at` directly against the image file. Streaming/random-access, never
  loads the whole disc into memory.
- `xenon::filesystem::ReadOnlyContentDevice` wraps any `ReadOnlyContentSource`
  (a `GdfxImageSource`, an `StfsPackageSource`, ...) as a full
  `xenon::filesystem::Device`, so the VFS, `xenon::xam::ContentManager`, and
  `xenon::filesystem::ContentProbe` never need format-specific code.
- `xenon::xam::ContentManager::mount_content_graph()` mounts a disc image (or
  a directory) at guest path `game:`, then symlinks `d:` and `dvd:` to it —
  this is the runtime-side "disc device" the spec's Parts 1/5 describe; it
  already existed and this pass only closed a `.xgd` mount-parity gap
  (`.iso`/`.dvd` were handled, `.xgd` was not, even though
  `ContentProbe`/the launcher's own probe already recognized it).
- `xenon::filesystem::read_all()` (`include/xenon/filesystem/read_only_content_device.hpp`)
  reads an entire file out of any `ReadOnlyContentSource` in one call — the
  primitive that lets a caller (title-update application, the preparation
  pipeline) get a complete in-memory `default.xex`/patch file out of a
  mounted disc image without ever writing a temporary file.
- Game identity (Title ID, Media ID, XEX version, disc number/count, and the
  effective executable's content hash) always comes from parsing the real
  XEX header via `xenon::xbox::parse_xex_image()`/`compute_effective_identity()`
  — never from the ISO's filename. `Ace Combat 6.iso` and
  `my_game_backup.iso` with identical internal title identity are treated
  identically everywhere in this pipeline.

Known, documented limitation (unchanged by this pass): `GdfxImageSource`
recognizes disc images via a fixed set of candidate game-partition byte
offsets, covering the layouts real single-partition Xbox 360 dumps use. It
does not implement full multi-partition XGD3/security-sector/video-partition
parsing. See `gdfx_image_source.hpp`'s own comments for exactly what is and
isn't modeled.

## 2. Managed game library

The launcher stores games under one user-configurable **library root**
(`PathService::libraryRootPath()`, setting key `frontend/paths/libraryRoot`),
defaulting to `Documents/Xenon Launcher`. Everything under it is **user data**
a person would reasonably want to see, back up, or move to another drive:

```
<Library Root>/
  Games/<managed-per-game-folder>/
    Media/            - disc images the user chose to move into the library
    DLC/<dlc-folder>/ - installed DLC (xenon::launcher::DlcService)
  Saves/<gameId>/     - per-game save data
  Profiles/           - user profiles
```

**Disposable/regeneratable data never lives here.** Generated C++, nested
build directories, the prepared-native-module cache, and preparation logs
live under `PathService::preparationCachePath()` (`<OS cache dir>/Preparation`
— see `PathService::cachePath()`, `QStandardPaths::CacheLocation`), and
installed module *packages* live under `PathService::defaultModulesPath()`
(app-data, not Documents) — these are program/derived data, not something a
user manages by hand, and must never bloat or clutter a Documents-based
library folder.

### Identity, not filenames or folders

A library entry's primary key (`gameId`) is derived from the game's own
**Title ID** (`"title-<hex>"`, from real XEX parsing — see
`LibraryService`'s `canonicalGameId()`), not from the source file's path or
name. Consequences:

- Re-importing the same title from a different disc copy, or after moving
  the source file, resolves to the **same** library entry and managed
  folder — it does not create a duplicate.
- A different region's disc, or another disc of a multi-disc title, for the
  **same** Title ID is added as an additional entry in that game's `media[]`
  list (each with its own Media ID/disc number/source path) rather than a
  second top-level library entry — multi-region media is never duplicated
  merely to "appear in multiple region folders."
- `contentPath` remains the currently-active disc for play purposes;
  `media[]` is the full set of known discs/regions for that title.

### Move into Xenon Library vs. Keep in current location

`Add Game` (an ISO or loose `.xex`) always asks which of the two the user
wants (`launcher/qml/LibraryPage.qml`'s import-mode dialog,
`ContentImportService::importGameContent(sources, moveIntoLibrary)`):

- **Keep in current location** (default — never moves a file the user did
  not explicitly ask to move): the library entry simply references the
  original path.
- **Move into Xenon Library**: the file is relocated into
  `<managed folder>/Media/` — same-volume rename when possible (atomic),
  otherwise copy-then-verify-size-then-delete-original (the original is
  **never removed until the copy is confirmed the right size**, so a
  multi-gigabyte disc image can't be lost to an interrupted move). A move
  failure does not fail the import: the content is already registered and
  playable from its original location either way.

### DLC / title updates: friendly names, real IDs retained

`xenon::filesystem::ContentProbe`'s STFS identification
(`XenonContentProbe::identifyDlc`) already extracts the package's real,
128-bit Xbox Content ID and cross-references it against a matched module's
DLC catalogue; when the catalogue names that exact content ID, the friendly
catalogue name is what the UI shows, while the real Content ID is retained
internally (`receipt.contentId`) as the canonical, trustworthy identity —
never inferred from a filename.

## 3. The prepared-module artifact cache

`include/xenon/recomp/artifact_cache.hpp` / `src/recomp/artifact_cache.cpp`
(target `xenon_recomp`). A content-addressed store for compiled
`xenon_game_module` shared libraries.

### Cache key (`ArtifactCacheKey`)

| Field | Source | Invalidates when... |
|---|---|---|
| `title_id`, `media_id` | `XexEffectiveIdentity` | the executable's own declared identity changes |
| `effective_image_hash` | `xbox::compute_effective_image_hash()` of the **effective** (decrypted/decompressed, title-update-patched if applicable) image | the base XEX or the applied title update changes in any way that changes code/data |
| `module_id` | the assigned Xenon module | a different module is assigned |
| `module_compatibility_version` | the module's **explicit** `manifest.json` `"compatibilityVersion"` field (`FileModuleHintProvider::compatibility_version()`, default `"1"`) — deliberately **not** the module's cosmetic display version | the module author bumps it (native hook/patch/ABI changes) |
| `hint_set_hash` | SHA-1 of the exact `AnalysisHintSetV2` JSON actually consumed | the module's analysis data for this exact revision changes, even if `compatibilityVersion` was not bumped |
| `abi_version` | `xenon::recomp::kArtifactAbiVersion` | Xenon's own generated-code/native-extension ABI changes |
| `target_arch`, `build_config` | host arch, Release/Debug | either changes |

This directly implements Part 16/17's policy: a **cover-art or README-only**
module update changes neither `module_compatibility_version` nor
`hint_set_hash` → no rebuild. A **display-version-only** bump changes
neither → no rebuild (there is no display-version field in the key at all).
An **analysis-hints** change always changes `hint_set_hash` → rebuild, even
if the module forgot to bump `compatibilityVersion`. A **native
hook/patch/ABI** change is exactly what `compatibilityVersion` exists for —
bump it explicitly in the module manifest. A **Xenon codegen/ABI** change
bumps `kArtifactAbiVersion`, invalidating every installed game's cache
regardless of module — separate from the launcher/runtime's own release
version, so an ordinary launcher update does not force this.

### Atomic stage → validate → promote (`ArtifactCacheStore`, `ArtifactStagingBuild`)

- `begin_staging(key)` returns a private directory, invisible to `lookup()`,
  distinct from that key's (possibly still-valid) committed entry directory.
- `commit()` **validates the compiled module for real** before promoting:
  loads it with the host loader (`LoadLibrary`/`dlopen`) and confirms it
  exports `Xenon_BindCompiledRegistry`, then unloads it immediately.
  Validation failure leaves the previous entry (if any) completely untouched.
- Promotion moves the old entry aside, renames the staging directory into
  its place, and only removes the old copy after the rename succeeds; if the
  rename fails, the old copy is restored. **The last known-good module is
  never destroyed until its replacement is verified in place** (Part 18).
- `discard()`/an unresolved `ArtifactStagingBuild`'s destructor removes the
  staging directory, leaving any existing valid entry untouched (Part 19).
- `clean_stale_staging()` removes leftovers from a killed/crashed previous
  run; the worker calls it once at startup.

Covered end-to-end by `tests/recomp/artifact_cache_tests.cpp` (digest
stability/invalidation, stage/commit/discard, rollback-on-invalid-module,
no-leftover-after-success) and `tests/recomp/xenon_prepare_worker_tests.cpp`
(real subprocess: first build, no-rebuild-when-unchanged, rebuild-when-hints-
change without disturbing the prior entry, deterministic cancellation).

## 4. The `xenon-prepare` worker

`tools/xenon_prepare.cpp` → executable `xenon-prepare`. Runs **out of
process** from the launcher (Part 10) — the launcher UI thread never blocks
on compilation, and a worker crash cannot crash the launcher.

Pipeline, entirely in-memory until generated source needs to hit disk:

```
content (ISO / directory / loose .xex)
  -> default.xex bytes (GdfxImageSource + filesystem::read_all() for an ISO;
     a plain read for a directory/loose file - never extracted)
  -> xbox::parse_xex_image()                              [base image]
  -> xbox::apply_title_update()  (if --title-update given) [effective image]
  -> xbox::compute_effective_identity()                    [cache key material]
  -> FileModuleHintProvider::provide()                      [AnalysisHintSetV2]
  -> ArtifactCacheStore::lookup()  -- Fresh? skip straight to done.
  -> recomp::load_and_analyze() (DriverOptions::pre_parsed_image - the
     already-resolved effective XexImage handed in directly; no re-parsing
     of a raw file, no ISO-awareness anywhere in xex_loader.hpp/driver.hpp)
  -> recomp::generate_project()          [generated C++ + CMakeLists.txt]
  -> nested `cmake -S/-B` + `cmake --build --target xenon_game_module`
  -> ArtifactCacheStore::begin_staging()/commit()           [atomic promote]
```

`DriverOptions::pre_parsed_image` (new field, `include/xenon/recomp/driver.hpp`)
is what makes "no full ISO extraction" and "XEX Loader V2 stays
format-agnostic" both true at once: the worker (which understands disc
formats) resolves an already-parsed, already-patched `XexImage` itself and
hands it to the driver directly; `load_and_analyze()` only reads from disk
via the older `DriverOptions::input` path when `pre_parsed_image` is unset
(zero behavior change for the existing CLI/tests, which still pass a path).

### CLI contract

```
xenon-prepare --content <path> --cache-root <dir>
              [--module <hint-package-dir>] [--module-id <id>]
              [--title-update <path>] [--config Release|Debug]
              [--status-file <path>] [--stop-signal <path>]
              [--recomp-root <path>] [--force] [--query]
```

`--query` never compiles — it only resolves identity and consults the cache,
printing one line of JSON to stdout:
`{"ok":true,"needsPreparation":bool,"status":"Fresh"|"Missing"|"Invalid","cacheKey":"...","titleId":"...","mediaId":"...","effectiveImageHash":"...","nativeExtensionPath":"..."}`
(or `{"ok":false,"error":"..."}`). This is what the launcher uses for the
fast "does Play need to build anything?" check.

Prepare mode (no `--query`) progressively overwrites `--status-file` with
`{"phase":"Inspecting|ApplyingTitleUpdate|AnalyzingExecutable|GeneratingSource|Compiling|Validating|Complete|Failed|Cancelled","percent":0-100,"task":"...","cacheKey":...,"titleId":...,"mediaId":...,"effectiveImageHash":...,"nativeExtensionPath":...,"error":...}`
— structured phases per Part 13, never inferred by scraping compiler stdout.
Exit codes: `0` success (including "already Fresh"), `1` usage, `2`
content/identity error, `3` analysis error, `4` source-generation error, `5`
build error, `6` validation/commit error, `7` cancelled.

### Cancellation (Part 19)

Creating `--stop-signal` requests cooperative cancellation. The worker checks
it at every phase boundary; during the (long) nested-CMake configure/build
step, cancellation terminates the **entire process tree** — a Windows Job
Object (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`) or a POSIX process group — not
just the immediate `cmake` process, since `cmake --build` spawns the actual
compiler/linker as further descendants that a plain `TerminateProcess`/`kill`
on `cmake` itself would leave running. Cancellation always leaves the
previous valid artifact (if any) untouched and removes any staging directory.

### Module hint package convention

The launcher's installed-module-package manifest (content identity, native
extension, DLC catalogue — `ModuleService`) and the Recomp Driver's
`FileModuleHintProvider` package (`manifest.json` + `revisions/<hash>/
analysis.json`, see `include/xenon/recomp/module_hint_provider.hpp`) are two
independent, narrower schemas that predate this pass. The bridge is a fixed
convention: a module that wants automatic preparation ships its hint package
at `<installed module directory>/xenon-analysis/`. `PreparationService`
(`launcher/src/services/preparation_service.cpp`) resolves this path and
passes it as `xenon-prepare --module`. A module that ships its own
pre-built native extension (`manifest.json`'s `"nativeExtension"` field) does
not need this at all — `PreparationService::usesAutomaticPreparation()`
returns false for it, and the existing (pre-existing, unchanged) shipped-
extension path is used exactly as before.

## 5. Launcher wiring

- **Import**: `ContentImportService::importGameContent()` no longer requires
  a compatible module to already be installed (Part 7) —
  `IContentProbe::identifyGame()` returns success with the disc/title
  identity even when zero installed modules match (only genuine *ambiguity*
  between multiple matching modules is a hard failure). The library entry is
  created with `moduleId` empty, `ready=false`, `status="Module required"`.
- **Deferred module resolution**: `IContentProbe::matchModule()` re-runs the
  same matching logic against an already-known identity, without touching
  the original content source. `ContentImportService::resolvePendingModule()`
  calls it for one library entry and applies a match via
  `LibraryService::applyIdentification()`. `LaunchService::ensureModuleResolved()`
  calls this automatically at the start of every Play attempt
  (`SessionController::preparePhase()`, before deciding whether automatic
  preparation is needed) — a module installed after import is picked up on
  the very next Play press, no re-import required.
- **First Play / automatic preparation**: `SessionController` already had a
  `Preparing` session state/UI card (`launcher/qml/LibraryPage.qml`); this
  pass makes it real for automatic-preparation modules instead of an
  effectively-instant passthrough:
  `PreparationService::usesAutomaticPreparation()` →
  `checkCache()` (fast, bounded, safe on the UI thread — never compiles) →
  if stale/missing, `beginPreparation()` (spawns `xenon-prepare` via
  `QProcess`, polls `--status-file`, emits `progress()`/`finished()`
  signals) → `SessionController` forwards phase/percent/task into
  `SessionRecord::progress_phase/progress_message/progress_percent`, shown
  live in the existing session card, and automatically advances to
  `Validating`/`Starting` on success — **the user never presses Play twice**.
  Cancelling a pending session while `Preparing` also cancels the worker.
  `LaunchService::configurationFor()` itself also consults the cache
  (`PreparationService::checkCache()`) so `validate()`/`startValidated()`
  resolve the freshly-prepared module correctly even if called independently
  of the `SessionController` state machine.
- A module that ships a native extension directly continues to work exactly
  as before — none of this is on its path.

## 6. Developer / loose-XEX workflow (preserved, Part 22)

Nothing above removes the direct developer path: `recomp-driver <game.xex>
[output] [--module <dir>] [--json]` (`tools/recomp_tools.cpp`), manually
building the generated CMake project, or launching a loose `default.xex`
from a directory, all continue to work unchanged. Automatic preparation is
an additional, optional path for the consumer `Add Game` → `Play` flow, not
a replacement for developer tooling.

## 7. Local compiler toolchain — STATUS: INCOMPLETE

Automatic preparation's `cmake --build` step still requires a real C++
toolchain to already be present on the end user's machine (MSVC/Build Tools
on Windows; a C++20 compiler + CMake on Linux) — Xenon does **not** bundle,
silently download, or install one. This is the one requirement from the
original task this pass does **not** satisfy:

> "Normal users must NOT be required to install Visual Studio Build Tools,
> CMake, Ninja, a compiler, Python manually."

What exists today: `generate_project()`'s emitted `CMakeLists.txt` already
minimizes what it needs from the toolchain (no graphics/audio/input, no
tests, `XENON_DEPENDENCY_MODE SYSTEM`), and `xenon-prepare` gives a clean,
specific failure (`unable to run cmake configure/build`, `exit code 5`)
rather than a silent hang or crash when no toolchain is present — but the
user must still resolve that themselves today.

**Why this is not implemented in this pass, concretely:** distributing a
compiler is an installer/legal/infrastructure decision, not a code change
this pass can make unilaterally:

- MSVC's headers/import libraries are not independently redistributable
  outside a Visual Studio/Build Tools installation — Xenon cannot legally
  vendor a "portable MSVC."
- A portable **clang-cl + lld-link** toolchain (LLVM's own releases are
  liberally licensed and redistributable) is the realistic path, but it
  still needs a C/C++ runtime and Windows SDK headers/import libraries to
  link against — either the Windows SDK (redistributable standalone, not
  part of Visual Studio) bundled alongside, or a `-fms-compatibility`
  build still targeting an already-installed SDK. This needs to be built,
  tested (does Xenon's existing MSVC-flag-conditional code — e.g. the
  `/constexpr:steps...` flags in `CMakeLists.txt`/`generate_project()` —
  need clang-cl equivalents?), and packaged by `cmake/Packaging.cmake`'s
  installer, none of which this pass has touched.
- On Linux, a modern GCC/Clang + CMake is normally provided by the distro's
  package manager already; the realistic path there is a first-run
  dependency check with a clear, actionable message (mirroring
  `cmake/Dependencies.cmake`'s existing `xenon_dependency_help()` pattern
  for SDL2/FFmpeg) rather than a bundled compiler.

**Recommended concrete next step** (not done here): (1) add a first-run
"toolchain check" to the launcher (locate `cmake` + a working C++ compiler,
surfacing `cmake/Dependencies.cmake`-style actionable guidance if missing,
before ever reaching Play); (2) prototype a clang-cl + lld-link + Windows
SDK portable bundle behind a new `XENON_BUNDLED_TOOLCHAIN` packaging option,
verified against a real `xenon-prepare` build end to end; (3) only after
that verification, wire the installer to offer it. None of the rest of this
pass depends on this being solved — `xenon-prepare` works correctly today on
any machine that already has a toolchain (exactly the environment used to
build and test everything in this pass).
