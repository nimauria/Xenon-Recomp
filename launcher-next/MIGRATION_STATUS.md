# Launcher migration status

6 November 2026. Feature implementation and release acceptance are separate. This is not a claim of 100% cross-platform acceptance or complete framework integration. The first production session connection is tracked in [INTEGRATION.md](INTEGRATION.md).

## Implemented

- Separate Qt-free CEF + React/TypeScript project. The legacy launcher remains locally under ignored `../launcher/`; root launcher builds now select this project.
- All 183 inventoried legacy methods have a disposition: 137 converted launcher flows and 46 explicit runtime-adapter seams, with zero partial method rows. See [MIGRATION_PARITY.md](MIGRATION_PARITY.md), [MIGRATION_PROPERTIES.md](MIGRATION_PROPERTIES.md) and [FEATURE_PARITY.md](FEATURE_PARITY.md). These counts are not a completion percentage.
- Original themes, backgrounds, accents, profiles, library/media/DLC, module lifecycle/settings, captures, jobs, notifications, support and recovery have native implementations and desktop checks.
- Per-game Graphics, Audio, Controls and Game Options: atomic saving, inheritance/reset controls and active-game locks. Resolution is game, then profile, then launcher defaults; module options remain isolated per game.
- Dedicated Updates navigation, visible install controls with availability explanations and Downloads' Ready to install section. Packages remain reachable after finished job records are cleared.
- File-backed English master and French/German/Spanish: 888 UI/recovery entries in all four languages. Native startup/crash recovery use the same catalogue. Many native diagnostics remain English; see [LOCALIZATION.md](LOCALIZATION.md).
- Explicit populated developer demo: four fictional games, two profiles, artwork, captures, notifications, jobs and functional local download/install/update packages. `npm run demo:fresh` uses separate data below `build/demo/`. Normal launches contain no seeded example library.
- Original Xenon Windows executable and caption/taskbar icons in multiple sizes. The native caption has been visually checked.
- Standalone Windows/Linux/macOS build/packaging entry points, licence notices and CI matrix. Marked Windows packages support automatic replacement; other platforms use manual replacement.

## Evidence

- Production web bundle builds; 37 frontend tests pass. Discover/Home adds a first-class catalogue, real-state attention shortcuts, immediate Downloads filters and keyboard command scrolling. See [REFINEMENTS.md](REFINEMENTS.md).
- 22 native Release suites pass on Windows and Linux. Linux desktop builds and sandbox-enabled Xvfb startup/four-language package checks have passed in the existing isolated Ubuntu container. The real Windows host consumed the launch configuration and reported the expected invalid-content error, with logs and saved host selection verified; this is not proof of a running game.
- Windows desktop checks cover 21 scenarios: demo, game settings, desktop, controller, languages, English/French crash recovery, profiles, media, modules, captures, migration, shell, services, library, jobs, network, presentation, cache, DLC and actions. The latest full run passed 19; controller and media then passed fresh isolated reruns. Initial media failure: Windows directory-rename access denied; originals were preserved. First-run controller timing remains worth monitoring.
- The actual Windows multiple-file chooser has been tested. Folder/save/cancel interaction needs broader acceptance.
- An isolated Windows 0.1.0-to-0.1.1 package update passed replacement, readiness receipt, backup retention and preserved profile/theme. Failure/rollback paths have native coverage. Normal build version remains 0.1.0.
- Actual renderer crashes, safe restart, recovery-origin isolation and data preservation pass, including French recovery copy.
- Managed cache relocation/reset, package installation after relocation, future downloads/artwork and long Windows paths pass native/desktop checks.
- Final-package startup medians are 0.40-0.51 seconds with 0/1,000 entries and 0.89-0.95 seconds with 10,000; the slowest sequential sample was 1.32 seconds. They measure interface readiness, not storefront artwork or physical input latency; see [PERFORMANCE.md](PERFORMANCE.md).

Migration-baseline Windows review package, before the Discover refinement: `build/packages/2f6635359cea43e5bb4e2929c4221932/xenon-launcher-next-windows-x64.zip`, SHA-256 `2e53f2ac6727ae6627b540f975ee5eec17243b1bb1742280f85a32dcb732cc63`. It includes Updates/install/demo presentation and branded icons. Packaged demo, localization and media checks passed; desktop/controller passed separately after a competing benchmark run exposed timing-sensitive failures. Those intermittent automation results remain documented, not erased by the successful rerun. The latest development desktop has separate passing Discover/Home, demo, shell, localization, controller and desktop checks; rebuild a package to distribute this newer slice.

Evidence stays in ignored build logs, `build/desktop-validation-*` and benchmark reports. Nothing has been published.

## Remaining acceptance

- macOS compile, app/helper startup, package icon, signing/notarization and native preference discovery. Source/CI exist; no Mac build/runtime result yet.
- Native diagnostic translations and translator review.
- Clean-machine distribution, broader Windows/Linux dialogs, lower-end hardware, large artwork libraries, physical controllers, screen readers and high DPI.
- Broader permission/filesystem and long-session fault coverage. A recoverable failure does not close a fault-tolerance gate.
- Preparation, content mounting, live input/filesystem/network management and module ABI/game compatibility acceptance. The real session transport is connected; unrelated service capabilities remain unavailable explicitly. Fixtures execute no game or Xenon code.

## Agreed sequence

Complete and verify standalone migration, then refine navigation/pages/features and quality of life using Steam, COGBox and GOG as references, then connect the launcher to the rest of Xenon. Per-game settings, Updates/install discoverability and the populated demo are the first refinement slice. Preserve original assets, honest capability states and game/profile isolation.
