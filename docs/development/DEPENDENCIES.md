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
- runtime release presets select `MANAGED` native dependencies and keep the retired installer disabled;
- `XENON_BUILD_INSTALLER=ON` now fails configuration with a retirement message;
- standalone launcher packaging uses `launcher-next` and its own pinned CEF/dependency tooling;
- no public release should be published until the runtime itself is declared release-ready.

Runtime dependency resolution, launcher packaging and future combined
distribution are separate concerns. The old combined Qt installer is not a
supported build path.

## Managed release dependencies

The future production release set is pinned in `tools/deps/manifest.json`. These pins do not force normal development builds to behave like installer builds:

| Dependency | Purpose | Release linkage |
| --- | --- | --- |
| SDL3 3.4.18 | Runtime window, audio output and controller input | static |
| Xenia-maintained FFmpeg | raw XMA frame decoding (`AV_CODEC_ID_XMAFRAMES`) | shared |
| Vulkan-Headers | Vulkan API declarations | headers |
| Vulkan-Loader | host Vulkan dispatch | shared |
| Microsoft DXC | HLSL → DXIL/SPIR-V shader compilation | shared |

The launcher uses a pinned CEF SDK fetched by `launcher-next/scripts/fetch-cef.mjs`.
Its web build requires Node.js (package minimum 22.12; the launcher guide uses
24 LTS). Qt is not a current dependency. See the
[launcher guide](../../launcher-next/README.md) for platform prerequisites,
licence collection and packaging.

## Audio and Xenia

Xenon Audio does not download or run Xenia. XMA packet/context state, looping,
sample accounting, mixing, memory coherency and xboxkrnl semantics are Xenon
code. The Xenia-maintained FFmpeg fork is used only for its raw XMA frame codec.

The FFmpeg revision stays pinned. The bootstrap applies the checked and hashed
upstream FFmpeg `mathops.h` fix for GNU binutils 2.41+ to an isolated build copy;
it never modifies the cached checkout. That patch is included in the matching
third-party source bundle. Linux and Windows builds keep the XMA decoder and
shared avcodec/avutil libraries enabled.

Windows developer `AUTO` builds may use the vetted static FFmpeg developer
bundle committed under `third_party/xenon-ffmpeg/windows-x64`. Production
runtime builds in `MANAGED` mode use the managed shared build instead of that
developer fallback.

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

### Windows runtime release-build prerequisites

The **build machine**, not the end user, needs:

- Visual Studio 2022 / MSVC developer environment;
- Python 3.9+, Git, CMake and Ninja;
- MSYS2 `bash` + `make` for the Xenia FFmpeg configure/make build;
- NASM for optimized FFmpeg x86-64 assembly (recommended).

The standalone launcher additionally needs its Node.js and CEF build inputs;
it does not use NSIS or Qt.

### Linux release-build prerequisites

The runtime build machine needs a C/C++ toolchain, Git, Python 3.9+, CMake,
Ninja, `make`, NASM and the platform development headers required by the native
dependencies. Launcher builds add Node.js, CEF, libcurl and CEF's desktop
libraries; use its guide and desktop workflow for the complete platform list.

These are build-host requirements only. They are not instructions for users of
the resulting package.

## Runtime staging

`xenon_stage_runtime_dependencies(target)` copies managed shared FFmpeg, DXC and
Vulkan loader files beside build-tree executables and gives Linux/macOS targets
an origin-relative runtime search path.

`xenon_install_runtime_dependencies(...)` installs the same runtimes and their
license material. The runtime host owns these rules for `cmake --install`;
this does not restore the retired combined installer.

The standalone launcher stages CEF and its own native dependencies through
`npm run desktop:package`. Its packages do not yet bundle the Xenon runtime
or preparation tool; see [integration status](../../launcher-next/INTEGRATION.md).

## Packaging boundary

The old `XENON_BUILD_INSTALLER=ON` contract is retired and rejected by root
CMake. Current launcher artifacts come from the
[standalone desktop packaging workflow](../../launcher-next/README.md#distribution-and-self-updating).
Combined launcher/runtime/tool distribution remains future work. Third-party
source availability is recorded in [THIRD_PARTY_SOURCE_OFFER.md](../../THIRD_PARTY_SOURCE_OFFER.md).

The planned [XeSS/FSR/DLSS integrations](../graphics/UPSCALING_FRAME_GENERATION_PLAN.md)
are not managed dependencies yet. Their package pins, capability checks and
redistribution notices must be added as part of implementation.
