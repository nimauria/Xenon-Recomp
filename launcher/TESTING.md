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

## V7.2 regression checks

- Profile list cards have consistent internal padding on all four sides; avatar and text never touch the selection border.
- Profile header actions reserve stable slots: activating/deactivating a profile must not shift Edit Profile, Duplicate, or More horizontally.
- Profile descriptions wrap to at most two lines in the profile header and are limited to 180 characters in Create/Edit.
- All modal editors dismiss on an outside click when clean. If dirty, the editor stays open and an unsaved-changes confirmation is displayed above it.
- Create/Edit footer actions stay right-aligned regardless of optional sections or text scale.
- Compact sidebar uses centered 44x44 navigation targets with an 8px rail inset and no oversized selection rectangles.
- The thin Windows-style chrome row contains only the window title/drag surface and window controls; branding/search/profile/help live on the second toolbar row.
- Maximize/restore remains clickable and restores to the previous windowed geometry.
- At 200% text size, body/caption text increases substantially while heading/control geometry scales more conservatively and pages remain usable without horizontal clipping.
- Theme background graphics are visibly present behind translucent panels, while foreground text remains readable.
- Theme-specific decorative rails/corners/dividers from the supplied side-asset pack are visible only as launcher chrome and never replace module/game artwork.

## v7.2.1 profile dismissal regression test

1. Open Create Profile and click outside without editing: the dialog should close.
2. Open Create Profile, type a valid name, then click outside: the editor should remain visible behind an unsaved-changes prompt.
3. Verify the prompt offers Create profile / Save changes, Discard changes, and Cancel.
4. Cancel returns to the still-populated editor. Discard closes without saving. Save/Create commits and closes.
5. Repeat with Edit Profile and Edit Content Locations.
6. With Create Profile open and a valid unique name entered, press Enter/Return: the profile should be created.
