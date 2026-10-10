# Xenon Launcher Changelog

This file consolidates the per-version front-end notes that previously lived in separate
`V7.x-NOTES.md` files. Newest first.

## v7.2.3

Startup-page hotfix.

- The launcher now defaults to the configured Startup page, which defaults to Library.
- Restoring the last-open page is now an explicit opt-in setting (`general/restoreLastPage`) and defaults to off.
- The previous development setting key is intentionally not reused so older test builds cannot keep reopening Settings because of stale persisted state.
- General Settings now labels this behaviour as **Open last page on startup** and explains that Library is the default.

## v7.2.2

Final front-end QA hotfix focused on profile-dialog interaction and top-level spacing.

- Profile Create/Edit/Paths dialogs now use Qt's reliable outside-press detection.
- A clean dialog closes when the user clicks outside it.
- A dirty dialog is restored and shows the existing Save / Discard / Cancel prompt.
- Enter/Return is a window-level default action for valid Create Profile forms.
- Settings-card trailing actions now share a consistent right edge, including switches.
- Custom window chrome and the Xenon toolbar have increased vertical and horizontal breathing room.

## v7.2.1

Profile dialog interaction hotfix.

- Clicking the dimmed area outside Create Profile, Edit Profile, or Edit Content Locations now always routes through the profile editor's close logic instead of relying on platform-specific Popup auto-close handling.
- Clean profile dialogs close immediately when clicking outside or pressing Escape.
- Dirty profile dialogs open a dedicated unsaved-changes prompt.
- The unsaved prompt offers **Save/Create**, **Discard changes**, and **Cancel** so closing can never silently lose edits.
- Pressing **Enter/Return** while creating a valid profile submits the form, including when focus is in the name or description fields.
- `XConfirmDialog` now supports an optional secondary action; existing confirmation dialogs are unchanged unless they opt into it.

## v7.2

Window chrome, profile ergonomics, responsive scaling, and fuller integration of Xenon-owned theme artwork.

- Added more internal padding to profile-list cards; kept the profile action cluster on one line with stable reserved button widths.
- Profile descriptions are limited to 180 characters, wrap to two lines in the selected-profile header, and elide beyond that.
- Create/Edit supports outside-click dismissal when clean and an explicit discard-changes confirmation when dirty; footer actions stay right-aligned.
- Profile IDs remain generated, immutable technical identifiers without a prominent copy action.
- Split the oversized top bar into a thin Windows-style title/chrome row and a separate Xenon application toolbar; reworked the frameless maximize/restore control.
- Tightened compact-sidebar width, centering, padding, and selected-state geometry.
- High text scaling now emphasizes typography rather than multiplying desktop geometry.
- Theme side-assets are packaged and used for rails, corners, dividers, HUD arcs, panel frames, status marks, overlays, and glow details; background choices remain scoped to their matching theme.
- Repaired duplicate trailing `</svg>` tags in the supplied side-strip SVGs before packaging.
- Game/module artwork remains separate from launcher chrome.

## v7.1

Visual regression fixes from the Windows Qt 6.10.3 test build, and integration of Xenon-owned theme artwork.

- Fixed the library-tile artwork sizing feedback loop; tiles are clipped with bounded artwork dimensions and real padding.
- Widened the selected-game detail surface's right gutter and DLC list/status padding; matched Game Information / Compatibility card heights in two-column mode.
- Kept DLC as an isolated nested scrolling surface with an as-needed scrollbar.
- Reworked profile-editor headers/footers (close top-right, Cancel/Save right-aligned); outside-click/Escape routes through dirty-state discard protection.
- Profile ID remains immutable/read-only without a prominent Copy action; avatar controls reflow at high text scale/narrow modal widths.
- Compact navigation uses centered icon-only buttons with more rail padding.
- Module information/capability cards align in two-column mode; capabilities render as wrapping pills.
- Module/catalog dialog close controls use anchored top-right placement.
- Removed editable module-catalog/update repository paths from Settings in favour of stable LauncherBridge update/catalog/install seams.
- Settings category navigation becomes a combo selector from 150% text scale upward.
- Accessibility typography uses non-uniform scaling (small UI text grows more than large headings).
- Top-bar/About branding uses the clean recolourable Xenon lockup with live scalable tagline text.
- Appearance exposes only theme-matched variants (Xenon Dark: Orbit/Tech/HUD; Carbon: Nebula/Tech; Industrial: Orbit/Tech; Light: Minimal). Raster artwork is launcher chrome only; game artwork remains module-provided.
