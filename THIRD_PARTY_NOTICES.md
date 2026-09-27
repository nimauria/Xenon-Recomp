# Third-party notices

Xenon Recomp itself is licensed under the MIT License. Components under
`third_party/`, the managed native dependency set, and Qt retain their own
licenses. Official packages install the corresponding license texts under
`share/licenses/Xenon`.

## Xenia-maintained FFmpeg / XMA decoder support

Audio V1 links dynamically to `libavcodec` and `libavutil` built from the
Xenia-maintained FFmpeg fork at revision
`15ece0882e8d5875051ff5b73c5a8326f7cee9f5`. Xenon uses this fork because it
exposes `AV_CODEC_ID_XMAFRAMES`, the raw Xbox 360 XMA frame decoder interface
needed by Xenon's own XMA context/packet implementation.

Xenon does **not** embed, download, or run the Xenia emulator. Only the pinned
FFmpeg fork is used as a codec dependency.

The selected FFmpeg configuration is distributed as shared libraries in public
packages. FFmpeg is primarily LGPL-2.1-or-later under this configuration. The
matching source archive is published alongside each official binary release;
see `THIRD_PARTY_SOURCE_OFFER.md`.

## Qt 6

The Xenon launcher uses dynamically linked Qt 6 desktop libraries and QML
modules. Official builds currently target Qt 6.10.3. The package contains the
Qt license files from the exact SDK used for the build, and the corresponding
Qt source-module archives used by the launcher are published alongside each
official binary release.

The Qt modules used by Xenon are selected from modules available under Qt's
open-source licensing terms. Public releases must preserve the bundled Qt
license material and the accompanying source-availability artifact.

## SDL2

Desktop Audio/Input uses SDL2. The managed production build pins SDL2 2.32.10
(the exact revision is recorded in `tools/deps/manifest.json`) and links it statically. SDL2 is distributed under
the zlib license. The upstream license text is retained with the package.

## Vulkan-Headers and Vulkan-Loader

The Vulkan backend is built against pinned Khronos Vulkan-Headers and the
Khronos Vulkan-Loader version recorded in `tools/deps/manifest.json`. Public
packages may ship the managed Vulkan loader, but do not and cannot bundle the
GPU vendor's Vulkan ICD/driver. A supported graphics driver remains an end-user
host requirement.

Vulkan-Headers and Vulkan-Loader are distributed under their upstream
Apache-2.0-compatible license terms; the managed dependency licenses are
included in the package.

## DirectX Shader Compiler (DXC)

Shader translation uses Microsoft's DirectX Shader Compiler release pinned in
`tools/deps/manifest.json` (currently v1.9.2607). The release bootstrap verifies
the SHA-256 of Microsoft's official DXC archive before installation. Public
packages carry the required `dxcompiler` runtime and Microsoft's accompanying
license/notice files.

## Microsoft runtime / Windows platform libraries

Windows packages may include Microsoft redistributable compiler/UCRT runtime
files using CMake's `InstallRequiredSystemLibraries` support and Qt's deployment
support. Direct3D 12 and GPU drivers remain Windows/driver-provided platform
components and are not copied from an SDK into the package.

## Source availability

Official Xenon binary releases are accompanied by a matching third-party source
bundle for redistributed LGPL components. See `THIRD_PARTY_SOURCE_OFFER.md` and
`docs/development/RELEASE_PACKAGING.md`.
