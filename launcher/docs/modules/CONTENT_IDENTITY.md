# Xenon module content identity

The launcher identifies user-provided Xbox 360 content through the generic Xenon
content probe and then matches the resulting Xbox metadata against installed
module manifests. Xenon never hard-codes a title-to-module mapping in the shared
runtime.

## Minimal compatibility form

Existing manifests may declare Xbox title IDs in `gameIds` / `game_ids`:

```json
{
  "id": "org.example.game-module",
  "name": "Example Game Module",
  "gameIds": ["1234ABCD"]
}
```

Only eight-digit hexadecimal values are interpreted as Xbox title IDs by the
content matcher. Other display-oriented game identifiers are ignored by this
matching path.

## Recommended identity form

For production modules, use explicit identities under `content.identities`:

```json
{
  "id": "org.example.game-module",
  "name": "Example Game Module",
  "content": {
    "identities": [
      {
        "titleId": "1234ABCD",
        "mediaIds": ["89ABCDEF"],
        "version": "2.0.12345.0",
        "discNumber": 1
      }
    ]
  }
}
```

`titleId` is the normal primary match. `mediaIds`, `version`, and `discNumber`
are optional refinements. When a refinement is present it is strict: a content
source that does not match it is not assigned to that module.

A module may declare multiple identity objects for regional/media revisions or
multi-disc releases. Title-specific identity data belongs to that game module,
not to Xenon Recomp.

## Matching safety

The launcher does not guess when no module declares the identified title, and it
rejects ambiguous equal-specificity matches across multiple installed modules.
This keeps content ownership explicit and prevents one module from silently
claiming another title.

Generation 4 identifies extracted/root game directories containing
`default.xex` and direct XEX2 selections. Generation 5 adds GDFX disc-image
identity, and Generation 6 adds single-file STFS package identity for DLC.

## DLC package identity

DLC catalogue entries should declare the exact STFS/XContent Content ID. The ID
is the package header's 20-byte content identifier rendered as 40 hexadecimal
characters; it is not the DLC filename or friendly display name.

```json
{
  "dlc": [
    {
      "id": "example-pack",
      "name": "Example Pack",
      "contentIds": [
        "101112131415161718191A1B1C1D1E1F20212223"
      ],
      "metadata": {
        "titleId": "1234ABCD",
        "mediaId": "89ABCDEF",
        "packageVersion": "2.0.12345.0"
      }
    }
  ]
}
```

`contentIds` is the ownership key and is required for production STFS matching.
The optional `titleId`, `mediaId`, and `packageVersion` fields only narrow a
content-ID match further. Xenon never assigns DLC merely because a filename or
display name looks familiar. This keeps all title-specific package identity in
the owning game module.
