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
`default.xex` and direct XEX2 selections. GDFX disc-image and STFS package
parsers are separate later providers and will feed the same identity contract.

## Runtime API requirements

Content identity and runtime capabilities are deliberately separate. A module
that owns a title may additionally declare the Xenon runtime APIs it expects.
For example:

```json
{
  "id": "org.example.game-module",
  "content": {
    "identities": [{ "titleId": "1234ABCD" }]
  },
  "runtimeApis": {
    "input": {
      "version": 1,
      "required": true
    }
  }
}
```

The launcher validates required API versions before launch and carries the
requirements in the generic launch contract. `runtimeApis.input` does not alter
content matching; it only describes the runtime contract required after the
module has been selected.

See `INPUT_API_V1.md` for the native module-facing Input contract.
