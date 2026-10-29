# Xenon Branding Pack v2

This pack contains clean vector branding assets for the Xenon launcher, GitHub presence, and future website usage under the Nimauria site.

## Core theme-ready SVGs
These use `currentColor` so the launcher or website can recolor them dynamically.

- `xenon-mark.svg` – symbol only
- `xenon-icon.svg` – symbol only alias
- `xenon-wordmark.svg` – full wordmark with a full X letter
- `xenon-lockup.svg` – symbol + wordmark
- `xenon-lockup-full.svg` – symbol + wordmark + tagline
- `xenon-favicon.svg` – simple symbol for favicon or lightweight embeds

## Launcher / application icons
- `xenon-app-icon.svg`
- `xenon-app-icon.ico`
- `xenon-app-icon-512.png`
- `xenon-app-icon-256.png`
- `xenon-app-icon-128.png`
- `xenon-app-icon-64.png`
- `xenon-app-icon-48.png`
- `xenon-app-icon-32.png`
- `xenon-app-icon-16.png`

## GitHub / website branding assets
- `xenon-github-social.svg` / `.png` – social preview image for GitHub repo/site cards
- `xenon-website-hero.svg` / `.png` – web hero/header banner
- `xenon-profile-mark.svg` / `.png` – profile/avatar-friendly symbol asset
- `xenon-favicon-64.png`, `xenon-favicon-32.png`, `xenon-favicon-16.png`

## Preview PNGs
- `xenon-mark.png`
- `xenon-lockup.png`
- `xenon-lockup-full.png`

## Integration
Use the core branding SVGs for UI and web. Keep the application icon assets for desktop/window/taskbar usage.

### Example
```css
.xenon-logo {
  color: var(--xenon-accent);
}
```

An example stylesheet is included as `web-theme-example.css`.
