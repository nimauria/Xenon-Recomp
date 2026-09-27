# Launcher updates

The update subsystem is owned by Launcher Core. QML only renders update state and invokes commands.

## Runtime flow

```text
Settings / automatic scheduler
        -> UpdateFeature
        -> UpdateProvider
        -> GitHubReleaseProvider
        -> GitHub Releases API
        -> compatible release asset
        -> package staging
        -> SHA-256 verification
        -> UpdateInstaller
        -> launcher exits
        -> external PowerShell helper replaces deployed files
        -> updated launcher restarts
```

`UpdateProvider` is replaceable. GitHub is the first provider, not part of the UI contract.

## GitHub release contract

The default repository is `nimauria/Xenon-Recomp`. It can be overridden at configure time with
`XENON_LAUNCHER_UPDATE_REPOSITORY`.

Release tags must be semantic versions such as:

- `v0.1.0`
- `v0.2.0`
- `v0.2.0-beta.1`

Draft releases are ignored. Prereleases are considered only when the user's update preference allows them.

The provider selects an exact platform/architecture asset. Windows x64 currently expects:

```text
xenon-launcher-windows-x64.zip
```

The GitHub workflow also publishes a human-readable `.sha256` companion file. The launcher itself verifies the
SHA-256 digest exposed for the release asset by the GitHub Releases API before installation.

## Building a release

`.github/workflows/release-launcher.yml` runs for tags beginning with `v`. It:

1. validates the tag as a semantic version;
2. installs Qt 6.10.3;
3. builds the launcher with the release version embedded;
4. runs `windeployqt`;
5. creates `xenon-launcher-windows-x64.zip`;
6. writes a SHA-256 companion file;
7. creates the GitHub Release (or replaces assets if the release already exists).

A release can therefore be published with:

```text
git tag v0.1.0
git push origin v0.1.0
```

The workflow intentionally builds the standalone launcher configuration while the wider Xenon runtime is still
under active development. When the release launcher begins linking the complete runtime, the workflow's CMake
feature switches should be changed to match the supported release configuration.

## Automatic checks

The launcher supports `At startup`, `Daily`, `Weekly`, and `Manual` intervals. The timestamp of the last successful
GitHub check is persisted by Launcher Core. Automatic network failures update status but do not display a toast.
A newly discovered update does notify the user.

## Installation and rollback

Windows installation is deliberately performed outside the running launcher. `UpdateInstaller` writes a temporary
PowerShell helper and launches it detached. The helper waits for the launcher process to exit, expands the verified
package, moves the old deployment to a sibling backup, moves the new deployment into place, starts the new launcher,
and removes the backup only after the replacement launcher survives its initial startup window. If replacement or
startup fails, the helper attempts to restore the backup and relaunches the restored launcher so failure does not
leave the application closed.

User profiles, settings, library metadata, saves, DLC and module data are stored outside the launcher deployment and
are not part of this replacement operation.

## Security boundary

Automatic install is refused when the selected GitHub release asset has no usable SHA-256 digest or when the
staged bytes do not match the advertised digest. HTTPS transport alone is not treated as package verification.
Code-signing can be layered on later without changing the QML update contract.
