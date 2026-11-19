# Game modules from the shared repository

Game module development and exported packages live in the shared
[Xenon-Modules](https://github.com/nimauria/Xenon-Modules) repository:

```text
development/<contributor>/<project>/   Source, hooks, tools and tests
games/<contributor>/<game>/             Exported launcher payload
```

For example:

```text
development/nimauria/project-gracemeria/
games/nimauria/project-gracemeria/
```

Development files stay under `development/` and are not downloaded by the
launcher. An exported game directory contains its catalogue metadata,
`xenon-module.json`, the explicit `package-files.json` list, generated
`package.json` hashes and any permitted runtime data, artwork or native module
binary. Commercial game content and game-derived native output remain local to
the user.

The launcher reads the version-2 catalogue and exported game payloads directly
from the `main` branch. It fetches the indexed files, verifies every declared
size and SHA-256 digest, and stages the selected module locally. Modules do
not need a separate game repository, GitHub Release, tag or uploaded ZIP.
Changed files are visible through their digests even when the module version
does not change.

To publish a reviewed package, module contributors work in `development/`,
declare the export inputs in `project.json`, then run the repository's export
and validation scripts:

```powershell
node scripts/export-module.mjs development/<contributor>/<project>
node scripts/export-module.mjs development/<contributor>/<project> --check
node scripts/validate-catalog.mjs --publish
node --test tests/workspace.test.mjs
```

For a tested prebuilt native module, pass its project-relative binary and host
platform to the export command. The exporter places it under
`bin/windows-x64/` (or the selected platform) and omits preparation hook
source that is already compiled into the DLL. Additional runtime dependencies
must be declared in the module project manifest.

The launcher itself continues to update from Xenon-Recomp releases. Module
installation and module updates use the shared Xenon-Modules catalogue and
per-file digest verification.
