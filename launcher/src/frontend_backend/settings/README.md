# Settings frontend backend

Settings is a launcher-owned vertical slice. QML renders controls, but it does not define the
authoritative option set, defaults, validation rules, theme palette, persistence keys or runtime
handoff.

```text
SettingsPage.qml / Theme.qml
            |
            v
      LauncherBridge
            |
            v
      FrontendBackend
       /           \
SettingsFeature   AppearanceFeature
      |                 |
SettingsCatalog         +-- theme catalogue / palettes
      |                 +-- accent catalogue / custom accent derivation
      |                 +-- corner styles
      |                 +-- backdrop variants and assets
      |                 +-- appearance reset
      v
SettingsService / QSettings
            |
            +--> LaunchConfiguration for runtime-relevant preferences
```

## Settings catalog

`schema/SettingsCatalog` is the single source of truth for normal UI-facing settings. Each entry
owns its category, type, default, allowed values and optional numeric range. A write from QML must
pass `SettingsFeature::setValidatedValue()`; unknown settings and invalid enum/boolean values are
rejected and numeric values are range-clamped before persistence.

The catalog currently owns launcher policies for general navigation, appearance presentation,
library behaviour, runtime/graphics, input, audio, updates, accessibility and developer tooling.
Path settings remain a dedicated workflow because changing a path may require profile migration,
module refresh or derived-path recomputation.

## Appearance engine

`appearance/AppearanceFeature` owns the theme-specific contract. QML no longer carries hard-coded
palette tables or backdrop compatibility rules. It asks the backend for:

- registered themes and complete semantic palettes;
- registered accent presets and derived strong/soft/text colours;
- validated custom accent colours;
- control corner styles;
- per-theme background variants and resource assets;
- the selected background for each theme.

`Theme.qml` remains the presentation token layer. It consumes one backend theme definition and
provides semantic colours, spacing, scaled typography and radii to controls. Adding a palette or a
new backdrop therefore does not require adding theme-selection logic to every page.

The `System` theme remains a real preference. The launcher resolves it to light or dark at runtime
from `QStyleHints::colorScheme()` and updates if the operating-system preference changes. On Qt
6.10+, the launcher also consumes the system high-contrast accessibility hint.

## Runtime handoff

Runtime-relevant settings are copied into `LaunchConfiguration` rather than requiring the Xenon
runtime to read launcher `QSettings`. The current contract includes renderer selection, shader-cache
policy, input preference/deadzone/rumble, master volume, focus mute and audio latency profile.

This preserves the boundary:

```text
QML -> Launcher settings -> typed LaunchConfiguration -> IRuntimeBridge -> Xenon services
```

Input, audio and graphics services can become live later without changing Settings QML.

## Reset semantics

Category reset is backend-owned. Appearance reset clears theme/accent/corners/backdrops and related
presentation preferences together. Path reset uses the same coordinated migration/refresh workflow
as an ordinary path change. Reset All resets all catalog values, appearance state, configured paths
and remembered launcher-page state; it does not delete profiles, library entries, modules, saves or
game content.

## QML-owned state

QML may own transient selection/search/dropdown state and visual layout calculations. It must not
own option registries, defaults, validation, persistence, theme definitions, capability filtering,
path migration, reset semantics or runtime configuration assembly.
