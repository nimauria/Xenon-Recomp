# Launcher refinement record

The standalone migration baseline was committed as `9aaf203`. Refinement remains ahead of real Xenon integration, as requested. Original themes/assets, native authority, per-game isolation and explicit fixture mode remain requirements.

## Discover and actionable Home

- Discover gives catalogue browsing a normal page alongside Home and Library. Modules remains the installed-module management page; its existing catalogue dialog remains available.
- Discover combines header search with All modules, Packages for this platform, Installed modules and Downloaded packages filters. Reset clears both query and filter. Installed/platform labels describe inventory, not runtime compatibility. Offline copy explains cached browsing and verified local packages.
- Discover is available in navigation, command search, launcher/profile startup settings and remembered native navigation. It shares existing verified download/install workflows rather than creating another installer.
- Home's Needs attention panel derives ready packages, updates for installed modules/the launcher, failed jobs and games with missing media sources from the authoritative snapshot. It disappears when empty. Download shortcuts open the relevant ready/failed filter and clear an old search.
- Library view selection is disabled during a pending native mutation, preventing an exposed control from sending a change while the command gate is occupied.
- Keyboard-selected command results scroll into view. The command palette loads on demand; the measured production entry bundle remains approximately 496 kB minified, with a separate 5 kB palette chunk. This is a bundle-size observation, not a new startup benchmark.
- Page changes return the document to its top before page-specific focus/deep links run, instead of inheriting the previous page's scroll position. Library collection scrolling remains managed by the library itself.
- All new copy is in the English/French/German/Spanish master catalogue. Fictional examples remain confined to the explicit demo.

Acceptance uses `npm run test:acceptance -- refinements demo shell localization controller desktop`, plus frontend/native suites. Refinements checks cover Home filter routing, catalogue filters/reset, remembered navigation, four languages, command scrolling and narrow layout. Review images are under ignored `build/refinement-evidence/`.

Verified locally on 6 November: production build and all 36 frontend tests; all 21 native suites on Windows and Linux; Discover/Home, demo installs, desktop appearance/recovery, command routing, controller menus and localization checks. The final scroll behaviour passed Discover/Home, command deep links, controller and language reruns. The first new test used the wrong existing Downloads label; its corrected version exposed and verified the immediate-filter fix. Evidence includes `build/discover-navigation-check.log`, `build/discover-native.log` and `build/discover-linux.log`. Mac acceptance remains open.

## Recommended next slices

1. Saved library views and collections: persist useful queries/sorts/filters by profile; allow rename/delete without moving game files. Keep collection counts and command search consistent.
2. A Storage page: put managed library destinations, cache locations and available space together. Scan sizes in background jobs, with explicit move previews and existing ownership/rollback rules.
3. A clearer selected-game workspace: group Overview, Game settings, Add-ons and Captures around a stable game header. Retain keyboard/controller focus and unsaved-change guards when switching sections.
4. A native single-step package workflow: chain release check, verified download and explicit install through persisted jobs. Design cancellation/retry and interrupted-state recovery before exposing an Install shortcut that starts multiple operations.
5. Release acceptance: remaining native translations, actual Mac build/runtime, physical controllers, screen readers, high DPI and lower-end/art-heavy performance. These remain open gates rather than fabricated completion.

These are scoped follow-ups, not implemented features. New pages should consolidate a real workflow; avoid adding navigation for its own sake. No real Xenon provider or runtime changes belong in this pass.

## Product references

- [Steam's library](https://store.steampowered.com/libraryupdate/) documents recent-game shelves, collections and unified game details. These inform hierarchy and future saved views.
- [GOG GALAXY](https://www.gog.com/galaxy) informs a personal collection and clearer discovery/management separation.
- [COGBox](https://github.com/Shagg88/COGBox) informs focused per-game settings and controller interaction. Its implementation and artwork are not copied.
