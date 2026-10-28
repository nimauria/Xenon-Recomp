# Xenon desktop design direction

Agreed direction, 28 October 2026: combine Steam's desktop structure, GOG GALAXY's collection browsing and COGBox's focused game/controller flow, expressed through Xenon's existing branding and theme system.

Set this direction during migration so every converted page uses the same hierarchy. Preserve the feature-parity checklist. Introduce larger new product features after the migration gates pass.

| Reference | Apply to Xenon |
|---|---|
| Steam | Persistent shell; clear Home versus Library; selected-game detail/action area; recent-game shelves and unobtrusive operation status |
| GOG GALAXY | Clean cover-grid browsing; useful search/filter/sort; consistent metadata and collection density; a strong sense of a personal local collection |
| COGBox | Optional focused box carousel; prominent Play and game-settings actions; controller-friendly focus/back behaviour; understandable content/setup failures |
| Existing Xenon | Original themes/backgrounds, accent customisation, profiles, generic module-driven settings, accessibility and honest runtime capabilities |

## Page responsibilities

- Home is the dashboard: recent real activity, recently added games, favourites and useful launcher status. Use a Continue action only when real session data justifies it.
- Library is the complete collection. Offer Focused, Grid and Carousel presentations over one authoritative data model. Keep selection, filtering and Play/Manage actions consistent between modes.
- Game details keep the primary actions together and group Overview, Add-ons, Module settings, Captures and advanced Properties by purpose.
- Modules and Profiles belong to management. Avoid exposing module installation machinery as normal game-library browsing.
- Downloads is the operation centre; its progress remains reachable from the application shell.
- Settings and Support stay at the bottom of navigation. Developer filesystem/network tools remain an optional group.

Navigation order: Browse (Home, Library, Captures), Manage (Modules, Profiles), Activity (Downloads), optional Developer tools, then Settings/Support. Preserve saved startup-page preferences.

## Shared visual rules

Use one set of semantic CSS tokens and accessible components. The original System, Xenon Dark, Carbon, Industrial and Light themes determine surfaces and decorative art. User accents recolour controls/vector branding; theme raster art remains specific to its palette. Module-provided cover/hero art belongs to games and remains optional.

Use restrained desktop spacing and readable text. Prefer a clear selected-game area over oversized empty artwork boxes. Keep actions discoverable by keyboard/controller as well as mouse. Collapse layout columns at narrow widths and increased text scale. Virtualize large collections and load art asynchronously without blanking valid images.

A coherent Xenon interface takes priority over reproducing individual applications pixel for pixel. Reference product behaviour and hierarchy; retain Xenon's own assets and implementation.

## Sequence

During migration: establish shell/navigation, shared controls, game presentation and theme application; keep converting the existing functions and testing their actual native behaviour.

After parity: assess genuinely new additions such as custom tags/saved collections, richer dashboard shelves, save backups or mod management. These remain separate from the current conversion checklist.

## References reviewed

- Steam's library organisation, recent games and collections: https://store.steampowered.com/libraryupdate/?l=english
- GOG GALAXY's library filtering, sorting, visual customisation and saved views: https://www.gog.com/galaxy
- COGBox's documented box carousel, per-game settings and controller navigation: https://github.com/Shagg88/COGBox

COGBox's README is the product reference; its implementation has not been fully audited. Reference ideas do not establish that an equivalent Xenon capability has been implemented or verified.
