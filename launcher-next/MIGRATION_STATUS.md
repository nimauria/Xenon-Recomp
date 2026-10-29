# Launcher migration status

29 October 2026. This is a progress record, not a declaration of full conversion.

| Plan area | Current result | Remaining gate |
|---|---|---|
| Separate project | New source/build/package tree under launcher-next; original launcher retained locally and ignored | Root build/installer/CI cutover only after parity |
| Baseline inventory | 55 QML components and 183 invokable methods mapped in MIGRATION_PARITY.md | Property getters and remaining partial flows still need final acceptance |
| CEF foundations | Windows Release DLL + sandbox bootstrap; local assets; command transport; packaged UI exercised | Clean-machine distribution; Linux live build; macOS helper bundles |
| Contracts | Versioned request/result envelope; native validation; correlated client with timeouts; initial DTO families; coalesced versioned native revision events | Live provider conformance during later Xenon integration; general request cancellation |
| Themes/assets | Original palettes, themes, accents, raster backgrounds, decor and branding converted; native rendering tested | Broader responsive/high-contrast/screen-reader regression matrix |
| Profiles | Native persistence, CRUD/duplicate, portable profile format, avatar copy/focal metadata; nine isolated runtime overrides, original limits/regions/startup pages and launch-config linkage | Unicode lowercasing parity and remaining avatar/editor interaction matrix |
| Settings | 55 old catalogue settings plus 8 appearance definitions; native and preview validation; sidebar mode, carousel wrap, grid density and remembered selection and startup-page precedence, individual resets and unavailable Rich Presence opt-in protection applied | Every remaining preference must be tied to its final behaviour; path relocation and unsupported-provider controls |
| Library/DLC | Focused/Grid/Carousel views; source references, filtering/sort, favourites, association, properties; verified managed game moves; DLC copy, catalogue selection, replacement backups, receipt checks and owned deletion; module cover/hero art through bounded native resource IDs; browsing state survives page changes | Multi-disc/media metadata parity, remaining property/actions acceptance and later live content adapter |
| Modules | Folder/ZIP install and rollback; original registry schema, cached/bundled catalogue, native HTTPS, artwork cache, semantic release selection, SHA-256 downloads/staged install, update history and batch checks; original settings aliases/defaults/options/ranges/reset; background manifest rescan and explicit game unlink | Full manifest/ABI compatibility, uninstall policy and final inventory acceptance cases |
| Jobs/Downloads | Native background ZIP and online catalogue/check/download jobs, persisted states/progress, cancellation/retry, interrupted-work recovery, bounded history and persistent shell access; renderer reload exercised during installation; packaged Windows self-replacement with verified staging, helper handoff, readiness receipt and rollback | Broader disk/permission fault and clean-machine coverage; Linux/macOS replacement is manual |
| Captures | Asynchronous coalesced scan, newest-first retention, virtualized thumbnail cards, image resource IDs, fit/zoom/next/previous/keyboard viewer, native image-app/folder opening and confirmed deletion | Broader image-format/large-image and accessibility matrix |
| Home/search/notifications | Real collection counts, favourite/recently-added shelves, direct game navigation, recent sessions with explicit fixture labels; separate ranked command search and notification/read-all/dismiss/action/toast controls | Playtime and activity aggregation parity |
| Support/recovery | Bounded redacted log ZIP, folder opening, render logs, visible native startup-failure window and crash marker; packaged safe-mode restart, clean shutdown marker, storage locking, actual renderer-crash reload/repeat-crash fallback with origin-isolated recovery links verified | Repeated unclean-exit policy and wider crash/recovery matrix |
| Controller/accessibility | Foreground gamepad navigation, keyboard shortcuts, focusable controls, text scale and contrast | Device-specific glyphs, native provider routing/rumble, spatial focus and screen readers |
| Runtime linkages | Production explicitly reports unavailable runtime; explicit separate fixture process, launch handshake/stop/history and reattachment tested | Complete adapter/cancellation/input conformance and eventual Xenon integration |
| Legacy state | Review/apply jobs, Windows read-only QSettings discovery, Linux INI reader, copied-data imports, backups, source/target revalidation, avatars and metadata retention, duplicate avoidance, cancellation and owned-file recovery | macOS automatic discovery, live Linux discovery and wider real-data acceptance |
| Remove Qt | New launcher has no Qt dependency | Old launcher source removed from version control at user request; local files retained; full parity remains open |

## Evidence so far

- Production React/TypeScript bundle builds.
- Fifteen frontend tests cover state invariants and converted appearance/assets.
- Fourteen native Release suites pass: managed library transfers/receipts, launcher replacement, host-service contracts, support redaction, core profiles, profile settings, application service, archives, module settings, install jobs, network jobs, capture index, legacy migration and runtime adapter. Coverage includes atomic state reuse, profile import/export, package update/rollback, typed settings, cancellation/retry, concurrent edits, interrupted commits, registry/release validation, semver, digest/staged-file verification, offline cache, support ZIPs and traversal/collision rejection.
- Packaged CEF smoke checks passed for native IPC, profile metadata, source/module registration, legacy module controls/choice validation, Home-to-Library navigation, preserved filters and debounced remembered selection after renderer reload, Carousel/art decoding, compact/expanded navigation, 640-pixel layout, all four explicit palettes, original backgrounds, appearance controls and high contrast.
- Packaged avatar import/focal metadata display, capture image viewing, support ZIP generation and forced failed-script error/reload checks pass.
- Packaged explicit fixture launch/Running handshake/asynchronous stop/dismissal/history and safe-mode restart with a clean shutdown marker pass. The fixture executes no game or Xenon code.
- Packaged job checks pass for invalid ZIP repair/retry, UI cancellation, unchanged source packages, preservation of the prior module, renderer reload during a 15,000-file retry, successful completion and clearing finished records. See [JOBS.md](JOBS.md) for lifecycle/recovery details and limits.
- Packaged network-fixture checks pass for catalogue/artwork, release check, verified download/install, bad digest rejection, update, rollback, update history and offline cache. The Windows WinHTTP transport separately fetched the real official registry with HTTP 200. Linux/macOS libcurl branches remain unvalidated here.

- Packaged profile editor/startup/reload/duplication checks pass, including all nine isolated values and inherited content/save/capture paths in the separate runtime fixture configuration.
- Packaged capture checks pass for 1,201 files, bounded card DOM, thumbnail decoding, sorting, explicit rescan/add/remove, viewer zoom/keyboard/navigation and confirmed deletion.
- Packaged copied-data migration review/apply, backup, original preservation, active profile, avatar decoding, theme restoration and renderer reload pass.
- Actual renderer crash tests pass for automatic first reload, repeated-crash fallback, rejection of privileged IPC from the fallback document, safe-mode restart and unchanged native data.

## Next work

Complete remaining partial flows in MIGRATION_PARITY.md, especially media/multi-disc metadata, activity aggregation, module compatibility/uninstall policy and property acceptance; then platform, accessibility and measured performance validation. Hold product improvements until migration gates pass. Keep partial, implemented and verified states distinct.

Latest packaged checks also pass for shell routing/notifications, input/filesystem/network fixture forms, managed game/DLC transfers and receipt checks, catalogue add-on search/pagination/multi-source replacement, individual setting reset, module discovery/rescan/unlink, and corrupt-state startup recovery.

A real isolated Windows 0.1.0-to-0.1.1 package update passed: old process exit, folder replacement, replacement interface-ready receipt, retained previous installation and preserved profile/theme. The default build version was restored to 0.1.0 afterward. No release was published. Updater tests additionally cover bad digests/packages, modified staged files, parent timeout, missing/bad receipt, early exit and rollback. Backup and failed-payload folders are retained; abrupt helper/machine failure may still require manual recovery.

The existing NVIDIA Workbench WSL environment can read this checkout but exposes no CMake/C++/pkg-config tools. No Linux toolchain was installed or distro configuration changed. Linux live builds and macOS desktop helper bundles remain open gates. Native Windows picker interaction also remains unverified; packaged source-selection tests use explicit fixture paths through native RPC.
