# Production release and installer pipeline

Xenon's public distribution model is **install the package and run it**. An end
user must not be asked to install Qt, SDL2, FFmpeg, DXC, the Vulkan SDK, Visual
C++ redistributables, or other development tooling manually.

The release build therefore differs deliberately from a developer build.

> **Development status:** this pipeline is intentionally opt-in while Xenon is still being developed. Normal builds leave `XENON_BUILD_INSTALLER=OFF`. Use the production presets only when preparing a release candidate or final public build.

## End-user contract

An official Xenon package contains:

- `xenon_launcher` and `xenon_runtime_host`;
- the production Memory, Filesystem/Kernel, Graphics, Audio and Input paths;
- the Vulkan backend and shader compiler support;
- the D3D12 backend on Windows;
- the XMA-capable shared FFmpeg runtime used by Xenon Audio;
- the DXC runtime;
- a managed Vulkan loader;
- the Qt/QML runtime used by the launcher;
- required Windows compiler/UCRT redistributables where applicable;
- third-party license and source-availability notices.

The package does **not** contain Xenia. Xbox semantics live in Xenon; the only
Xenia-derived external runtime dependency is the pinned FFmpeg fork providing
`AV_CODEC_ID_XMAFRAMES`.

The following remain external host requirements because they are hardware/OS
components rather than redistributable Xenon dependencies:

- a supported 64-bit Windows or Linux operating system;
- a supported GPU driver. For Vulkan this includes the vendor Vulkan ICD;
- game/module content the user is legally entitled to provide.

## Reproducible dependency mode

Release presets force:

```text
XENON_DEPENDENCY_MODE=MANAGED
XENON_BUILD_INSTALLER=ON
```

`cmake/Packaging.cmake` refuses to configure a production package if core
runtime subsystems are disabled or if the managed Vulkan/DXC targets were not
resolved. This prevents a launcher-only or host-dependent build from being
mistaken for a release.

Pinned native dependencies are described by `tools/deps/manifest.json` and are
provisioned by:

```bash
python tools/deps/bootstrap.py ensure --triplet windows-x64
python tools/deps/bootstrap.py ensure --triplet linux-x64
```

DXC release archives are SHA-256 checked. Git dependencies are checked out at
exact commits and the generated dependency stamps bind the install to the
manifest hash and platform triplet.

## Windows package

The Windows release preset generates:

- an NSIS installer for normal users;
- a ZIP payload as a portable/diagnostic alternative.

Qt's CMake deployment support collects Qt/QML runtime files. Xenon's managed
FFmpeg, DXC and Vulkan loader runtimes are placed beside the executables.
Microsoft compiler/UCRT redistributables are included through the supported
CMake/Qt deployment mechanisms.

The GitHub release workflow performs a silent installation of the generated
NSIS installer into a temporary directory and runs `tools/release/verify_install.py`
against the **installed result**, not merely the build tree.

Authenticode signing is supported through the repository secrets
`WINDOWS_SIGNING_CERT_BASE64` and `WINDOWS_SIGNING_CERT_PASSWORD`. Validation
builds may be unsigned, but public production releases should configure a
trusted code-signing certificate.

## Linux package

The Linux release preset generates a Debian package plus a TGZ diagnostic
archive. CI installs the generated `.deb` onto the release runner, verifies the
installed `/usr` tree, and checks the launcher/runtime ELF dependency closure
before removing the package again. Xenon stages a private Qt runtime under `lib/xenon-recomp`, uses a
`qt.conf` relative to the launcher, and rewrites ELF RUNPATHs with `patchelf` so
a matching system Qt installation is not required.

CPack `dpkg-shlibdeps` is used for ordinary host libraries. GPU vendor Vulkan
ICDs are intentionally not bundled and remain a graphics-driver requirement.

## Release verification

`tools/release/verify_install.py` makes the installed payload a release gate. It
checks for:

- launcher and runtime host;
- FFmpeg `avcodec`/`avutil` runtime libraries;
- DXC runtime;
- managed Vulkan loader;
- Qt Core and a desktop platform plugin;
- `qt.conf` on Linux;
- Xenon and third-party notices;
- license material for FFmpeg, DXC, Vulkan, SDL2 and Qt.

Missing runtime or compliance files cause the release job to fail.

## LGPL source bundle

The binary release is not the only artifact. `tools/release/collect_source_offer.py`
creates:

```text
Xenon-Third-Party-Sources-<version>.zip
```

containing the exact Qt source module archives used by the launcher and an
archive of the pinned Xenia-FFmpeg revision. Qt archives are checked against the
SHA-256 values in `tools/release/source_offer_manifest.json`.

The GitHub release workflow treats this source bundle as a required dependency
of the publish job, so a binary release cannot be published if the source bundle
failed to build.

## CI release flow

For a `vX.Y.Z` tag the workflow performs, in order:

```text
provision pinned dependencies
        ↓
configure production release preset
        ↓
build full Xenon + launcher
        ↓
run CTest
        ↓
stage install tree
        ↓
verify installed runtime
        ↓
create installer/package
        ↓
installer/package smoke checks
        ↓
build third-party source bundle
        ↓
publish binaries + checksums + sources
```

A failure at any stage blocks publication.

## Development-phase publication gate

The release workflow is safe to keep in the repository before Xenon is ready for public distribution. Manual `workflow_dispatch` runs build and validate production artifacts, but tag-triggered jobs/publishing are gated by the repository variable:

```text
XENON_PRODUCTION_RELEASES_ENABLED=true
```

Leave the variable unset or `false` during active development. When the project is ready for its first production release, enable it and create a semantic `vX.Y.Z` tag. The workflow then builds, tests, installs/smoke-tests, verifies compliance material and publishes the validated artifacts.
