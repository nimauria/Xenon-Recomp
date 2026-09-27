# Xenon FFmpeg/XMA dependency

This directory contains Xenon's vetted **Windows x64 developer fallback** for
FFmpeg `libavcodec` and `libavutil` used by Audio V1.

It is not a runtime dependency on the Xenia emulator. The libraries are built
from the Xenia-maintained FFmpeg fork because that fork exposes the raw Xbox 360
hardware-frame interface `AV_CODEC_ID_XMAFRAMES` required by Xenon's XMA
contexts.

## Pinned provenance

- Xenia build-harness revision: `95a5c3ee250f80c3b9d139658649d9ffb6db3eec`
- FFmpeg revision: `15ece0882e8d5875051ff5b73c5a8326f7cee9f5`
- FFmpeg libavcodec API version: `58.134.100`
- Triplet: `windows-x64`
- Linkage: Release static libraries

`windows-x64/XenonFFmpegBundle.cmake` records the revision/triplet/linkage
identity and SHA-256 hashes. CMake verifies that metadata before accepting the
bundle.

## How it is used now

`XENON_DEPENDENCY_MODE=AUTO` may use this committed bundle on Windows x64 so a
normal development checkout does not need to build FFmpeg first.

Official release presets use `XENON_DEPENDENCY_MODE=MANAGED` and deliberately
skip this static fallback. `tools/deps/bootstrap.py` then builds the same pinned
FFmpeg fork as shared libraries and stages them beside Xenon binaries. That path
is also used to make Linux releases reproducible.

The static bundle can be rebuilt with:

```powershell
./tools/rebuild_xenon_ffmpeg.ps1
```

The script also regenerates the identity manifest.

## Licensing

FFmpeg is primarily LGPL-2.1-or-later in this configuration. See
`windows-x64/licenses/`.

The committed static bundle is intended for development convenience. Public
release packaging should use the managed shared build described in
`docs/development/DEPENDENCIES.md`, which keeps LGPL redistribution substantially simpler.
