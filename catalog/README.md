# Xenon Module Catalog

`modules.json` is the launcher-facing discovery catalog for public Xenon modules.
It is intentionally **not** a game/module runtime manifest.

The catalog only tells Xenon Launcher where a module is published and which
GitHub Release asset matches each supported host platform. Runtime capabilities,
game IDs, DLC definitions, module settings and other game-specific information
remain owned by the installed module manifest.

## Catalog schema

The root document currently uses schema `xenon.module-catalog`, version `1`.
Each module entry contains:

- `id` — stable Xenon module ID. This must match the ID inside the installed package.
- `name`, `type`, `description` — catalog presentation metadata.
- `repository` — public GitHub repository in `owner/repository` form.
- `publisher`, `license`, `verified`, `tags` — catalog metadata.
- `releaseAssets` — exact GitHub Release asset names keyed by host, for example
  `windows-x64` or `linux-arm64`.

A catalog entry does not distribute commercial game data. It points only to the
open-source Xenon module package.

## Release rules

Module releases should use semantic-version tags such as `v0.1.0` or
`v0.2.0-beta.1`. The launcher ignores draft releases and ignores prereleases
unless the user enables the module prerelease channel.

The release must attach the exact package name declared in `releaseAssets`.
The updater requires GitHub's SHA-256 release-asset digest before it will stage
or install the package.

See `docs/modules/GITHUB_MODULE_RELEASES.md` for the publisher workflow and
package layout expected by the current launcher.
