# Xenon FFmpeg/XMA dependency

This directory contains Xenon's vetted Windows x64 build of the FFmpeg
`libavcodec` and `libavutil` libraries used by Audio V1.

It is **not** a dependency on the Xenia emulator at runtime. The decoder build
is derived from the Xenia-maintained FFmpeg fork because that fork exposes the
raw Xbox 360 hardware-frame interface `AV_CODEC_ID_XMAFRAMES` required by
Xenon's XMA context implementation.

## Pinned provenance

- Xenia build-harness revision: `95a5c3ee250f80c3b9d139658649d9ffb6db3eec`
- FFmpeg submodule revision: `15ece0882e8d5875051ff5b73c5a8326f7cee9f5`
- FFmpeg libavcodec API version in this bundle: `58.134.100`
- Windows architecture: x86-64
- Build type: Release static libraries
- Toolchain used for the current bundle: MSVC 14.51 / Visual Studio Build Tools 2026 (`v145`)

Only FFmpeg's installed/public headers (plus generated `avconfig.h`) are kept in
`include/`; decoder source files are not copied into Xenon's include tree. The
Windows bundle includes a SHA-256 identity manifest and CMake verifies the two
archives plus `codec_id.h` before using them.

## CMake behavior

On Windows x64, Xenon automatically prefers
`third_party/xenon-ffmpeg/windows-x64`. A developer may override this by
setting `XENON_AUDIO_FFMPEG_ROOT` to another compatible FFmpeg installation.
The configure step still compiles a capability probe for
`AV_CODEC_ID_XMAFRAMES`, so an incompatible library/header combination fails
immediately rather than producing a silent/no-audio runtime.

Other platforms currently use a compatible system/external FFmpeg build.

## Licensing

FFmpeg is licensed primarily under LGPL-2.1-or-later; see the license files in
`windows-x64/licenses/`. These archives were built from the non-GPL Xenia FFmpeg
configuration. Xenon's MIT license does not replace FFmpeg's license.

Anyone distributing a statically linked Xenon binary must satisfy the LGPL
requirements applicable to static linking, including the user's ability to
relink with a modified FFmpeg. A production release process should therefore
publish the corresponding FFmpeg source/revision and relinkable Xenon objects,
or move the packaged release to shared FFmpeg libraries. Keep this notice and
the bundled license texts with distributed artifacts.
