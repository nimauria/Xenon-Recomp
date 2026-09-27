# Xenon launcher theme backgrounds

These files are launcher UI chrome/background art. They are not game artwork and must not be used as substitutes for module-provided game tile or hero art.

Theme mappings used by `ThemeBackdrop.qml` and Settings → Appearance:

- **Xenon Dark**
  - `theme-xenon-dark-orbit.png` — Orbit
  - `theme-xenon-dark-tech.png` — Tech
  - `theme-xenon-dark-hud.png` — HUD
- **Carbon**
  - `theme-carbon-nebula.png` — Nebula
  - `theme-carbon-tech.png` — Tech
- **Industrial**
  - `theme-industrial-orbit.png` — Orbit
  - `theme-industrial-tech.png` — Tech
- **Light**
  - Minimal vector treatment only; no dark raster background is reused.

The reusable SVG accents in `../decor/` are layered at low opacity and are selected to match the base theme (blue for Xenon Dark, green for Carbon, amber for Industrial, neutral overlays where appropriate).

Accent colour customisation still recolours Xenon vector branding and semantic UI controls. Raster theme art remains theme-specific rather than being hue-shifted at runtime.
