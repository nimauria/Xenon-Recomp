# Library frontend backend

The Library UI is presentation-only. This slice projects persistent game records and coordinates
library actions without teaching QML about module manifests, Xbox content formats or runtime APIs.

```text
LibraryPage.qml / GamePropertiesDialog.qml
                |
                v
          LauncherBridge
                |
                v
   LibraryFeature / DlcFeature / GamePropertiesFeature
                |
                +--> LibraryService
                +--> DlcService
                +--> ModuleService
                +--> ContentImportService -> IContentProbe (framework seam)
                |
                v
          LaunchConfiguration
```

## Game records

A production Library record references the user's original game content and stores launcher metadata.
It also has a stable launcher-managed directory. Removing the record removes metadata only; it never
deletes the source game or the managed DLC directory.

The managed path is:

```text
<configured Games path>/<game-id>/
```

Save and screenshot paths are resolved per game underneath the active profile/configured roots.

## DLC catalogue and storage

The selected game's module supplies the DLC catalogue. `ModuleService::dlcCatalog()` is the only
adapter that interprets provisional raw manifest keys. It normalizes them to launcher fields such as
`dlcId`, `name`, `description`, `version` and `contentIds`. This deliberately avoids committing
Library QML or storage code to Project Gracemeria's final manifest schema.

Launcher-managed DLC is stored as:

```text
<configured Games path>/<game-id>/DLC/<DLC display name>/
```

The display name is sanitized only where required for filesystem safety. If two manifest entries
collapse to the same safe folder name, the later collision gets a stable DLC-ID suffix.

`DlcService` owns install-state projection, safe paths, `.xenon-dlc.json` receipts, verification,
removal and the runtime-facing installed-DLC list. It refuses deletion outside the managed DLC root.

## Import boundary

Production game and DLC import has exactly one route: `ContentImportService -> IContentProbe`.
The default `UnavailableContentProbe` intentionally changes nothing until the Xenon filesystem/content
subsystem can identify Xbox content.

The future implementation only needs to provide these operations:

```text
identifyGame(source, installed module candidates) -> normalized module/title metadata
identifyDlc(source, gameId, normalized manifest catalogue) -> dlcId + receipt metadata
materializeDlc(source, launcher-prepared destination, identification)
```

Launcher Core then owns registration and storage:

```text
Game:
probe identify -> validate installed module -> register persistent Library record

DLC:
probe identify dlcId
  -> DlcService::prepareInstall()
  -> probe materializes into the exact prepared folder
  -> DlcService::commitInstall()
  -> receipt + UI state + launch mount become available
```

This means the Project Gracemeria manifest and Xbox package parser can change later without changing
the Library page or its QML contracts.

## Launch contract

The launch contract includes both the launcher-managed game root and only installed, verified-path
DLC entries. Runtime-facing DLC data is normalized to `dlcId`, `name`, `path`, `version`,
`contentIds` and receipt metadata; raw manifest objects do not cross the runtime boundary.

## Context actions

`actions/LibraryActionCatalog` and `dlc/actions/DlcActionCatalog` are the authoritative action policy
for visible action buttons and right-click menus. QML owns pointer hit-testing and popup placement only.
This keeps actions such as Play, Properties, Module Settings, file management, verification, DLC
management and Library removal consistent regardless of how the user opens the menu.

Right-click hit areas are attached to the visible ListView viewport rather than its scrolling
`contentItem`, so row detection remains correct after scrolling and unused list space can expose the
background Add/Import actions.

## QML-owned state

QML may still own ephemeral search text, selection, menu/dialog state and visual filtering. It must
not own persistent library metadata, manifest interpretation, DLC install state, paths, verification,
import logic or launch-contract assembly.
