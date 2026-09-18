# Xenon Launcher Front-end Test Checklist

This checklist is for the current launcher-only milestone. It exercises QML/front-end behaviour without requiring the Xenon runtime backend.

## Build

From the repository root in an x64 Visual Studio developer PowerShell/VS Code terminal:

```powershell
.\launcher\scripts\build-launcher.ps1 `
  -QtRoot "C:\Qt\6.10.3\msvc2022_64" `
  -TestMode `
  -Clean `
  -Deploy `
  -Run
```

`-TestMode` only enables the Developer fixture selector. Use **Settings → Developer → Launcher fixture data** to switch between an empty launcher, generic fictional fixtures, and the Project Gracemeria UI preview.

## Window and navigation

- Resize from the minimum window size through maximized state.
- Verify minimize, maximize/restore, and close remain aligned and clickable.
- Collapse/expand the sidebar from the top control.
- Confirm `Sidebar layout = Compact` visibly produces icon-only navigation.
- Confirm `Ctrl+1`, `Ctrl+2`, `Ctrl+3`, and `Ctrl+,` navigate to Library, Modules, Profiles, and Settings.
- Confirm `Ctrl+K` focuses the top contextual search field.
- Confirm the Xenon mark/lockup recolours when the accent changes.

## Library

- Search filters the game list.
- Switching fixtures rebuilds the library without restarting the launcher.
- DLC only shows a scrollbar when required.
- Mouse-wheel/touchpad scrolling over DLC does not move the outer game-detail page at the same time.
- Game Information and Compatibility panels grow with their contents instead of overflowing their borders.
- `Module Settings` opens a schema-driven settings dialog and remains usable with more settings than fit vertically.
- Removing a fixture game names the game explicitly and does not imply that user game files are deleted.

## Modules

- Search filters installed modules from the top search bar.
- Import Module and Browse Module Catalog live at the bottom of the module list.
- Modules with no artwork do not reserve an empty artwork panel.
- Action buttons wrap with padding rather than touching card edges.
- Module Settings generates controls from the fixture schema.
- Browse Module Catalog clearly separates open-source module packages from user-provided commercial game content.

## Profiles

- Create Profile opens a modal editor.
- Clicking outside/Escape closes a clean editor immediately.
- Clicking outside/Escape after editing asks whether unsaved changes should be discarded.
- Profile ID is generated internally and is not editable in Create/Edit.
- Profile image can be selected locally and displays in the profile list/header/menu.
- `Edit Profile` shows identity/preferences but does not expose path fields.
- `Edit Paths` shows only content-location overrides.
- Create Profile may optionally enable custom content locations; leaving them disabled inherits Settings → Paths.
- Duplicate creates a unique display name and a new immutable profile ID.
- Active profile cannot be deleted until another profile is activated.

## Settings and accessibility

- Theme, accent, corners, background graphics, and background intensity update immediately.
- System theme follows the OS light/dark preference.
- Accessibility toggles do not change unrelated toggles.
- Test text sizes at 100%, 125%, 150%, 175%, and 200%.
- At high text scale, multi-column layouts should stack and remain scrollable rather than clipping.
- Settings search filters categories and moves to a matching category where appropriate.
- Settings → Paths includes profile storage as well as game/save/module/screenshot/cache locations.

## Diagnostics

- Developer diagnostics contain build/runtime/system/display/graphics/UI state useful for reproducing bugs, without dumping local content/configuration paths.
- About exposes a shorter user-facing summary.
- Copy buttons put the corresponding text on the clipboard.


## V7.1 visual regression checks

- Library tiles keep artwork, title/module text and status fully inside the selected/unselected card at every supported text scale.
- The selected-game detail view retains a visible right gutter; DLC status text and its scrollbar never touch the window edge.
- Game Information and Compatibility panels align at the top and use matching card heights in two-column mode.
- Maximize/restore switches between the Windows work area and normal windowed state; resize hit-zones are disabled while maximized.
- Profile editor/path editor close buttons remain top-right. Clicking outside a clean editor closes it; clicking outside a dirty editor opens the discard confirmation.
- Profile editor footer actions remain right-aligned in Create, Edit Profile and Edit Paths modes.
- Compact sidebar icons are centered with consistent padding and do not show empty text boxes.
- At 150–200% text scale, Settings category navigation switches to a single combo box and detail cards reflow vertically.
- Top-bar and About branding use the scalable lockup without an unreadably small embedded tagline.
- Appearance only offers background variants belonging to the active theme: Xenon Dark (Orbit/Tech/HUD), Carbon (Nebula/Tech), Industrial (Orbit/Tech), Light (Minimal).
- Module information/capability cards align in two-column mode and capability badges wrap rather than clipping.
