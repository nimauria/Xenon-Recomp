# Launcher Modules Backend

The Modules vertical slice owns launcher-side module discovery, state, actions,
settings, catalog discovery and package updates. QML presents this state but does
not decide how modules are discovered, trusted, updated or linked to Library.

```text
ModulesPage / ModuleCatalogDialog
            |
       LauncherBridge
            |
       ModulesFeature
       /       |        \
ModuleService  |    ModuleActionCatalog
       |       |
ModuleImportService
       |       |
ModulePackageInstaller
               \
 GitHubModuleCatalogProvider
               |
       ModuleUpdateService
               |
     ModulePackageInstaller
```

## Enable/disable semantics

Disabling a module does not remove dependent games from Library. `LibraryFeature`
projects the live module state into each game entry. A dependent game remains
visible with `ready = false` and status `Module disabled`; the Library surface can
enable the module directly. Removing the managed module directory similarly
leaves the Library record visible with status `Module missing`.

This is deliberate: Library records represent the user's imported/identified
game content, while module state represents whether Xenon currently has the
code needed to launch that content.

## Right-click actions

`ModuleActionCatalog` is the single policy source for both context menus and the
visible `...` action menu. QML does not maintain a second list of enable/update/
verify/remove rules.

## Local import

Local package import is also launcher-owned. `ModuleImportService` accepts either
a ZIP package or an unpacked module directory, discovers exactly one supported
module manifest, then installs it into the managed Modules directory. Local
imports do not need to be listed in the official catalog, but an unmanaged
module has no guessed network update source.

## Xenon Modules registry versus module manifest

The official discovery source is `nimauria/Xenon-Modules`. Its root
`catalog.json` (`xenon.module-catalog`, version 1) contains repository-relative
paths such as `modules/org.nimauria.project-gracemeria.json`. The provider fetches
and validates each `xenon.module-entry` separately, then normalizes publisher,
GitHub repository and per-host release-asset metadata for the launcher.

The registry is metadata only. It maps a stable module ID to its public GitHub
repository and exact per-host release asset names; it does not host module
binaries. The installed module manifest remains authoritative for game IDs, DLC
definitions, settings, capabilities and future runtime/module ABI data.

## Updater trust boundary

The module updater accepts network packages only from an official registry entry.
It resolves the entry's declared GitHub repository, selects the exact host asset,
requires a GitHub-provided SHA-256 digest, streams into launcher staging, verifies
the digest, validates the extracted module ID, and requires the package manifest
version to match the selected semantic GitHub release before replacement.

`ModuleService::installFromDirectory` owns managed-path replacement. Failed
replacement is transactionally restored immediately. A successful official update
retains the previous installation under `.xenon-rollbacks/<module-id>/`, with the
newest three snapshots retained for explicit user rollback.

`ModuleUpdateHistoryStore` persists the newest 100 update events globally (up to
20 per module), including checks, downloads, installs and rollbacks. History is
stored under the launcher's local application-data directory and is exposed to
QML through `ModulesFeature`/`LauncherBridge`; QML does not own updater history.

The launcher updater and module updater intentionally do not share release state
or channels. `updates/prerelease` controls Xenon Launcher releases;
`updates/modulePrerelease` controls module releases.

## Registry source

Official registry: `https://github.com/nimauria/Xenon-Modules`

The provider fails safely for unsupported registry schema versions and does not
infer title-specific runtime data from registry metadata.

## Registry presentation data and artwork cache

The official Xenon Modules registry can carry launcher-facing game metadata in addition to package
discovery data. The launcher preserves normalized `launcher`, `game`, `dlc`, compatibility and
capability fields from each registry entry instead of discarding them after module-store discovery.

Registry artwork paths are downloaded by `catalog/assets/ModuleCatalogAssetCache` into the configured
launcher cache. Cache metadata stores the source URL plus ETag/Last-Modified values, allowing refreshes
to use conditional GitHub requests. Library QML receives local `file://` URLs only.
