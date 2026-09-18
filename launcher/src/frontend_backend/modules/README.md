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

## Catalog versus module manifest

The official catalog is discovery/update metadata only. It maps a stable module
ID to its public GitHub repository and exact per-host release asset names. The
installed module manifest remains authoritative for game IDs, DLC definitions,
settings, capabilities and future runtime/module ABI data.

## Updater trust boundary

The module updater accepts packages only from an official catalog entry, selects
the exact host asset, requires a GitHub-provided SHA-256 digest, verifies the
streamed package, then checks the extracted module ID before replacement.
`ModuleService::installFromDirectory` owns managed-path replacement and rollback.

The launcher updater and module updater intentionally do not share release state
or channels. `updates/prerelease` controls Xenon Launcher releases;
`updates/modulePrerelease` controls module releases.
