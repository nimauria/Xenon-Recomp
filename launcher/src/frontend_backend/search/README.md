# Command palette

The command palette is the launcher-wide discovery and command-routing layer.

It aggregates launcher-owned projections from Home, Library, Modules, Profiles, Settings and a small set of safe launcher commands. QML renders the returned result maps but does not invent search entries or execute backend operations directly.

`CommandPaletteFeature::search()` performs ranked, token-based matching. Empty queries return a bounded quick-action set; non-empty queries search games, installed modules, profiles, settings categories and commands together.

Execution is split deliberately:

- backend operations (update checks, support bundle generation, Discord, session stop) execute in `CommandPaletteFeature`;
- UI navigation emits `navigationRequested(page, target, section)` and `Main.qml` routes that request into the already-loaded feature page;
- individual feature pages expose narrow selection/open helpers such as `selectGameById()` rather than the palette reaching into their internal models.

Safe Mode excludes Library, Module and Profile entities and suppresses commands that would re-enable production/network work during recovery.
