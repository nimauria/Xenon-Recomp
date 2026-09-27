# Xenon managed dependencies

Xenon has one centralized native dependency resolver in `cmake/Dependencies.cmake`
and one provisioning tool in `tools/deps/bootstrap.py`. Individual subsystems
must not invent separate download or package-selection logic.

## Modes

`XENON_DEPENDENCY_MODE` accepts:

- `AUTO` — developer mode. Prefer explicit/already-managed dependencies, allow a
  compatible system package, then bootstrap a missing pinned dependency.
- `MANAGED` — reproducible mode. Use explicit roots or Xenon's pinned managed
  dependency tree only. Official release presets use this mode.
- `SYSTEM` — use host-installed/explicit packages only and never bootstrap.

Managed dependencies are installed below:

```text
.xenon/deps/<platform>-<arch>/
```

Each install has a `.xenon-dependency.json` stamp containing its exact revision,
triplet and dependency-manifest hash. A stale install is rejected rather than
silently reused.


## Development versus production

Xenon is still under active development. The dependency system is therefore split deliberately:

- normal developer presets keep `XENON_BUILD_INSTALLER=OFF` and may use `AUTO` or `SYSTEM` dependency resolution;
- the production presets are opt-in and set `XENON_BUILD_INSTALLER=ON` plus `XENON_DEPENDENCY_MODE=MANAGED`;
- installer/package validation is only enabled for those production presets;
- no public release should be published until the runtime itself is declared release-ready.

This means the production machinery can live in the repository now without making every in-progress developer build stage Qt, create installers, or enforce release-compliance gates.

## Managed release dependencies

The future production release set is pinned in `tools/deps/manifest.json`. These pins do not force normal development builds to behave like installer builds:

| Dependency | Purpose | Release linkage |
| --- | --- | --- |
| SDL2 2.32.10 | Audio output and SDL2 input | static |
| Xenia-maintained FFmpeg | raw XMA frame decoding (`AV_CODEC_ID_XMAFRAMES`) | shared |
| Vulkan-Headers | Vulkan API declarations | headers |
| Vulkan-Loader | host Vulkan dispatch | shared |
| Microsoft DXC | HLSL → DXIL/SPIR-V shader compilation | shared |

The launcher additionally uses a pinned Qt release in release CI. Qt deployment
is handled by the install/package graph rather than by the native dependency
bootstrap because it is already a first-class CMake/Qt SDK.

## Audio and Xenia

Xenon Audio does not download or run Xenia. XMA packet/context state, looping,
sample accounting, mixing, memory coherency and xboxkrnl semantics are Xenon
code. The Xenia-maintained FFmpeg fork is used only for its raw XMA frame codec.

Windows developer `AUTO` builds may use the vetted static FFmpeg developer
bundle committed under `third_party/xenon-ffmpeg/windows-x64`. Production
installers never use that fallback: release presets force the managed shared
build instead.

## Vulkan

A public build does not require the end user to install the Vulkan SDK. Xenon's
release builder provisions pinned Vulkan-Headers and Vulkan-Loader itself and
bundles the redistributable loader runtime.

The **GPU vendor Vulkan driver/ICD is not a Xenon dependency and is not bundled**.
It must come from the user's AMD/Intel/NVIDIA/other graphics driver installation.

## DXC

DXC uses Microsoft's official v1.9.2607 release assets on Windows and Linux.
The bootstrap verifies the published archive SHA-256 before extracting it and
normalizes its headers/import libraries/runtime files into the managed prefix.

## Provisioning

Examples:

```bash
python tools/deps/bootstrap.py status
python tools/deps/bootstrap.py ensure
python tools/deps/bootstrap.py verify
python tools/deps/bootstrap.py ensure --only xenia-ffmpeg
python tools/deps/bootstrap.py ensure --only vulkan-loader
python tools/deps/bootstrap.py ensure --only dxc
```

`vulkan-loader` automatically provisions its pinned Vulkan-Headers prerequisite.
`--offline` refuses network access and succeeds only when the exact pinned source
or archive is already cached.

### Windows release-build prerequisites

The **build machine**, not the end user, needs:

- Visual Studio 2022 / MSVC developer environment;
- Python 3.9+, Git, CMake and Ninja;
- MSYS2 `bash` + `make` for the Xenia FFmpeg configure/make build;
- NASM for optimized FFmpeg x86-64 assembly (recommended);
- NSIS for the installer;
- Qt 6.6+ SDK (release CI currently pins Qt 6.10.3).

### Linux release-build prerequisites

The build machine needs a C/C++ toolchain, Git, Python 3.9+, CMake, Ninja,
`make`, NASM, `patchelf`, Debian packaging tools, and the normal X11/XCB
development headers needed to build the Vulkan loader/Qt desktop package.

These are build-host requirements only. They are not instructions for users of
the resulting package.

## Runtime staging

`xenon_stage_runtime_dependencies(target)` copies managed shared FFmpeg, DXC and
Vulkan loader files beside build-tree executables and gives Linux/macOS targets
an origin-relative runtime search path.

`xenon_install_runtime_dependencies(...)` installs the same runtimes and their
license material. The runtime host owns these install rules so normal
`cmake --install` and CPack packages contain the same native payload.

Qt is deployed separately through Qt's supported CMake deployment API on
Windows. Linux packages add a private Qt runtime and QML/plugin set using
`tools/release/stage_qt_linux.py` and relative RUNPATHs.

## Public installer contract

`XENON_BUILD_INSTALLER=ON` is deliberately strict. Packaging fails unless:

- `XENON_DEPENDENCY_MODE=MANAGED`;
- Memory, Filesystem/Kernel, Graphics, Vulkan, DXC, Audio and Input are enabled;
- the launcher, Vulkan backend and DXC compiler target exist;
- D3D12 is available for Windows production builds.

See `docs/development/RELEASE_PACKAGING.md` for installer creation, CI validation and
third-party source availability.
