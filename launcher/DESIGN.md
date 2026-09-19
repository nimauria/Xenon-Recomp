# Xenon Launcher Design Notes

This document records the reasoning behind the launcher UI so the front-end can evolve without becoming a collection of one-off screens.

## Product principles

1. **Activity first, Library close behind.** Home earns a primary destination by aggregating real launcher/session history; Library, Modules, Profiles, and Settings remain the operational destinations. Rare actions stay contextual rather than becoming permanent navigation items.
2. **Local content only.** Xenon does not present commercial games or DLC as downloadable content. Games and game-owned content enter through local import; modules identify and validate them.
3. **Module-driven presentation.** Game title, description, tile art, hero art, DLC catalogue, supported IDs/regions, and game-specific compatibility data are module-provided metadata.
4. **Front-end remains useful before the backend exists.** Unsupported actions explain what service is missing instead of silently failing. Test mode uses explicitly fictional fixtures.
5. **Settings apply immediately.** Ordinary preference changes take effect at once. Confirmation is reserved for destructive operations.
6. **Accessibility is product quality.** Keyboard navigation, focus visibility, high contrast, non-colour status labels, and text scaling are part of the default design rather than a late add-on.

## Qt implementation rules

- Use Qt Quick Controls before inventing a new interaction control.
- Xenon custom controls are based on the cross-platform Basic style rather than restyling native controls.
- Keep QML focused on presentation and immediate interaction. Discovery, validation, persistence, launching, updates, and runtime work belong in C++ services/models.
- Prefer typed QML properties and typed C++ settings accessors when the type is known.
- Persist state in models/services rather than inside ListView delegates.
- Use explicit interaction signals (`clicked`, `activated`, `userToggled`) for writes instead of generic property-change signals.
- Use `ListView` for scalable collections and enable delegate reuse where delegates are stateless.
- Do not block the GUI/render thread with filesystem scanning, hashing, imports, downloads, shader work, or module discovery. Those services must become asynchronous when implemented.
- Use layouts and implicit sizing; fixed dimensions are reserved for bounded visual assets and deliberate desktop breakpoints.
- Use vector/path icons for launcher chrome instead of platform-dependent Unicode/emoji icons.

## Interaction rules

- `Ctrl+K` / `Ctrl+F`: open/focus the launcher-wide command palette.
- `Ctrl+H`: Home.
- `Ctrl+1`: Library.
- `Ctrl+2`: Modules.
- `Ctrl+3`: Profiles.
- `Ctrl+,`: Settings.
- Settings is pinned to the bottom of primary navigation.
- The profile control in the title bar is a quick switcher only. Profile creation/editing belongs on the Profiles page.
- Nested DLC scrolling consumes wheel/touchpad input so the outer page does not scroll at the same time.
- Scrollbars use `AsNeeded` and should not be permanently visible when content fits.

## Responsive/accessibility targets

- Normal desktop target: 1280×720 and larger.
- Layout must remain usable at 200% launcher text scaling.
- Controls that become crowded should stack rather than clip.
- Navigation can collapse into a compact rail at constrained widths.
- Status cannot rely on green/yellow/red alone; always pair colour with text or an icon.
- Every interactive custom control should expose an accessible name and keyboard focus path.
- Stable `objectName` / automation IDs are used on important controls for future UI automation.

## Settings information architecture

Settings are grouped into focused categories rather than one enormous page. Each category should normally contain a small number of related controls. Categories may be capability-gated so unfinished backend systems do not expose misleading options.

Global paths and profile overrides are intentionally separate:

- **Settings → Paths:** launcher-wide defaults (games, saves, profiles, modules, screenshots, cache).
- **Profiles → Edit:** optional per-profile overrides for game/save/screenshot locations.

## Branding and module-distribution contracts

- Core Xenon UI branding uses the theme-ready SVG assets from `resources/branding/`. These assets use `currentColor`; `XenonBrand.qml` asks `LauncherBridge` for a recoloured in-memory SVG data URL so the mark/wordmark follows the active accent without maintaining duplicate colour variants.
- Application/taskbar icons use the dedicated branding-pack icon resources rather than treating the UI lockup as an OS icon.
- Game artwork is never a Xenon branding requirement. Tile/hero art is optional module-provided presentation data.
- A future public module catalog should contain module metadata and links to versioned open-source module releases. Each game module remains independently versioned and Xenon never distributes commercial game executables, assets, title updates, or DLC through that catalog.
- Module-specific settings are schema-driven (`id`, `label`, `description`, `type`, default/options, and future visibility/restart metadata). The runtime/module service owns the schema; generic Xenon QML owns only its presentation.

## V7 interaction/responsive decisions

- Top-bar search is launcher-wide and routes directly to games, modules, profiles, settings categories, and launcher commands.
- Profile IDs are generated, immutable internal identities. The editor exposes user-owned profile fields, optional avatars, and optional path overrides instead.
- Profile editor path overrides are collapsed unless explicitly enabled or the user chooses Edit Paths. Outside click/Escape closes a clean editor but asks before discarding dirty state.
- DLC is a bounded nested scrolling surface. Wheel/touchpad input is consumed by the DLC list when it can scroll, preventing the outer game-detail page from moving simultaneously.
- At increased text scale or constrained width, multi-column cards stack. Fixed heights are avoided for text-bearing information panels.
- Developer diagnostics are reproducibility-focused; About is user-focused. Local filesystem locations are intentionally not part of copied diagnostics unless a future explicit path-report action requests them.

## References used for the v7 review

- Qt — Best Practices for QML and Qt Quick: https://doc.qt.io/qt-6/qtquick-bestpractices.html
- Qt — Performance considerations and suggestions: https://doc.qt.io/qt-6/qtquick-performance.html
- Qt — ListView: https://doc.qt.io/qt-6/qml-qtquick-listview.html
- Qt — ScrollBar: https://doc.qt.io/qt-6/qml-qtquick-controls-scrollbar.html
- Qt — WheelHandler: https://doc.qt.io/qt-6/qml-qtquick-wheelhandler.html
- Qt — Image (`sourceSize` for bounded user images): https://doc.qt.io/qt-6/qml-qtquick-image.html
- Qt — Accessibility: https://doc.qt.io/qt-6/accessible.html
- Microsoft — Windows app development best practices: https://learn.microsoft.com/windows/apps/get-started/best-practices
- Microsoft — Navigation design basics: https://learn.microsoft.com/windows/apps/design/basics/navigation-basics
- Microsoft — Guidelines for app settings: https://learn.microsoft.com/windows/apps/design/app-settings/guidelines-for-app-settings
- Microsoft — Dialogs and flyouts: https://learn.microsoft.com/windows/apps/develop/ui/controls/dialogs-and-flyouts/
- GitHub — About releases: https://docs.github.com/repositories/releasing-projects-on-github/about-releases

## V7.1 theme-art and responsive refinements

Theme artwork is launcher chrome, not game artwork. Raster files live under `resources/backgrounds/` and are named by the base theme they belong to. Appearance only exposes variants valid for the current effective theme. The reusable SVG side/corner/overlay assets under `resources/decor/` are layered above those backgrounds at low opacity; game tile/hero artwork remains a separate, module-owned contract.

Accessibility scaling is typographic rather than a uniform zoom. Caption/body text grows more strongly than title/display text, and crowded multi-column views switch to stacked layouts or compact selectors at higher text scale. This follows the Windows approach of preserving readable hierarchy and reflow instead of doubling every desktop dimension.

Update/catalog repository addresses are implementation details rather than user preferences. QML invokes stable `LauncherBridge` update/catalog/module-operation seams; the future frontend service can resolve official GitHub release/catalog endpoints, download asynchronously, verify packages, and report progress without exposing absolute URLs in Settings.

Profile IDs remain immutable internal identifiers so profile display names can change safely. They are visible for troubleshooting/diagnostic correlation but are not presented as a primary copy/action workflow.
