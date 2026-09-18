# Publishing Xenon Modules through GitHub Releases

Xenon Launcher uses two independent GitHub-backed update systems:

1. **Launcher updates** come from `nimauria/Xenon-Recomp` releases.
2. **Module updates** come from the repository declared for each entry in
   `catalog/modules.json`.

This separation lets a game module ship on its own schedule and keeps its
runtime/game manifest independent from the central discovery catalog.

## Project Gracemeria example

The catalog currently maps `org.nimauria.project-gracemeria` to
`nimauria/Project-Gracemeria`. A stable Windows x64 release should therefore be
published with a semantic-version tag such as:

```text
v0.1.0
```

and should contain the exact asset:

```text
project-gracemeria-windows-x64.xenonmod.zip
```

The ZIP may contain the module files directly at its root, or one top-level
folder containing the module. The current launcher accepts the manifest names
already understood by `ModuleService` (`xenon-module.json`, `module.json`, or
`manifest.json`). The manifest's module ID must be exactly:

```text
org.nimauria.project-gracemeria
```

The updater deliberately does not define the rest of Project Gracemeria's final
manifest here. Game IDs, DLC catalog data, launch configuration, capabilities
and other runtime details remain part of the module/framework contract and can
evolve without changing the GitHub catalog format.

## What the launcher does

For an official catalog module the launcher:

```text
catalog entry
    -> module GitHub repository
    -> GitHub Releases
    -> newest allowed semantic version
    -> exact host asset
    -> stream download to launcher staging
    -> SHA-256 verification
    -> extract to a temporary directory
    -> verify expected module ID
    -> backup installed module
    -> replace module
    -> refresh discovery
    -> remove backup on success
```

If replacement fails, `ModuleService` restores the previous managed module
folder. Games that depend on a disabled module remain visible in Library and are
marked **Module disabled**; games whose module was removed remain visible and are
marked **Module missing**.

## Suggested release automation

Project Gracemeria can eventually use its own GitHub Actions workflow. The exact
build/package commands should be added only after its final module build output
is known, but the release half can follow this shape:

```yaml
name: Release Xenon module

on:
  push:
    tags:
      - "v*"

permissions:
  contents: write

jobs:
  windows-x64:
    runs-on: windows-2022
    steps:
      - uses: actions/checkout@v4

      # TODO: configure and build Project Gracemeria's module output.
      # The produced package directory must include its Xenon module manifest.

      - name: Package module
        shell: pwsh
        run: |
          Compress-Archive `
            -Path path/to/module-output/* `
            -DestinationPath project-gracemeria-windows-x64.xenonmod.zip

      - name: Publish release asset
        shell: pwsh
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          gh release view "${{ github.ref_name }}" *> $null
          if ($LASTEXITCODE -ne 0) {
            gh release create "${{ github.ref_name }}" --generate-notes
          }
          gh release upload "${{ github.ref_name }}" `
            project-gracemeria-windows-x64.xenonmod.zip --clobber
```

Do not add a personal GitHub token to Xenon Launcher or to the module package.
Public catalog/release reads work anonymously; the publishing workflow uses the
repository-scoped `GITHUB_TOKEN` provided by GitHub Actions.

## Adding another official module

Add one entry to `catalog/modules.json` with a unique stable module ID, a public
GitHub repository, and exact asset names for each supported host. Once that
catalog change is available on the Xenon-Recomp `main` branch, launcher clients
can discover it on the next catalog refresh. Installed local modules not listed
in the official catalog continue to work, but the launcher will not guess an
update source for them.
