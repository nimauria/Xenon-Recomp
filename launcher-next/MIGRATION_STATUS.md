# Launcher migration status

28 October 2026. This is a progress record, not a declaration of full conversion.

| Plan area | Current result | Remaining gate |
|---|---|---|
| Separate project | New source/build/package tree under launcher-next; original launcher retained locally and ignored | Root build/installer/CI cutover only after parity |
| Baseline inventory | 55 QML sources and 183 legacy bridge declarations recorded | Every declaration still needs an individual acceptance-case disposition |
| CEF foundations | Windows Release DLL + sandbox bootstrap; local assets; command transport; packaged UI exercised | Clean-machine distribution; Linux live build; macOS helper bundles; renderer-crash restart policy |
| Contracts | Versioned request/result envelope; native validation; correlated client with timeouts; initial DTO families | Complete runtime/content/input contracts; revisioned push events and cancellation |
| Themes/assets | Original palettes, themes, accents, raster backgrounds, decor and branding converted; native rendering tested | Broader responsive/high-contrast/screen-reader regression matrix |
| Profiles | Native persistence, CRUD/duplicate, portable profile format, avatar copy/focal metadata | Full avatar/editor interaction acceptance tests; Unicode normalization parity; per-profile runtime overrides |
| Settings | 55 old catalogue settings plus 8 appearance definitions; native and preview validation | Every preference must be tied to its final behaviour; path relocation and unsupported-provider controls |
| Library/DLC | Source references, filtering/sort, virtualized collection, favourites, association, properties and local add-on references | Content identification/integrity adapter, managed import/delete workflows, media/artwork metadata parity |
| Modules | Folder registration, manifest settings, ZIP install, prior versions and rollback | Online catalogue/releases/downloads, complete manifest schema/options validation, uninstall policy and update-state parity |
| Jobs/Downloads | Completed local-install records projected into UI | Cancellable durable queue, live progress, retries, online providers and interrupted-job recovery |
| Captures | Native folder scan, resource serving, viewer and confirmed deletion | Virtualized bounded thumbnails, zoom interaction, scan coalescing and refresh coverage |
| Home/search/notifications | Real local state projections and basic command routing | Full ranking/actions, playtime and activity aggregation parity |
| Support/recovery | Summary export/ZIP, folder opening, render logs, startup fallback, crash marker and safe-mode restart code | Full redacted log collection; restart/crash scenarios verified in packaged app |
| Controller/accessibility | Foreground gamepad navigation, keyboard shortcuts, focusable controls, text scale and contrast | Device-specific glyphs, native provider routing/rumble, spatial focus and screen readers |
| Runtime linkages | Production explicitly reports unavailable runtime | Fixture-host lifecycle, adapter conformance and eventual Xenon integration |
| Legacy state | Old portable profile files accepted; existing storage is preserved | Reviewable QSettings/library/profile/avatar migration with backups and interruption tests |
| Remove Qt | New launcher has no Qt dependency | Old launcher source removed from version control at user request; local files retained; full parity remains open |

## Evidence so far

- Production React/TypeScript bundle builds.
- Nine frontend tests cover state invariants and converted appearance/assets.
- Native Release core, application-service and archive tests pass, including atomic state reuse, malformed requests, profile import/export, module package update/rollback, support ZIPs and traversal/case-collision rejection.
- Packaged CEF smoke checks passed for native IPC, profile metadata, source/module registration, setting validation, navigation, renderer reload, all four explicit palettes, original background decoding, appearance controls, high contrast and a failed-script error screen followed by reload.
- Latest avatar/recovery/package additions require the next packaged smoke pass before being marked verified at UI level.

## Next work

Complete the fixture/runtime adapter seam and cancellable job/update services; finish page-specific parity against the inventory; add legacy migration and platform packaging validation. Keep placeholder, partial, code-complete and verified states distinct.
