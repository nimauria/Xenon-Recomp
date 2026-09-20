# Third-party notices

Xenon itself is licensed under the MIT License. Components under `third_party/`
retain their own licenses.

## FFmpeg / XMA decoder support

Audio V1 uses `libavcodec` and `libavutil` derived from the Xenia-maintained
FFmpeg fork, pinned to FFmpeg revision
`15ece0882e8d5875051ff5b73c5a8326f7cee9f5`, because it exposes
`AV_CODEC_ID_XMAFRAMES` for raw Xbox 360 XMA hardware frames.

FFmpeg is primarily LGPL-2.1-or-later in the configuration used here. See:

- `third_party/xenon-ffmpeg/README.md`
- `third_party/xenon-ffmpeg/windows-x64/licenses/LICENSE.md`
- `third_party/xenon-ffmpeg/windows-x64/licenses/COPYING.LGPLv2.1`

## SDL2

Desktop Audio/Input uses SDL2. If SDL2 is not installed for a source build,
CMake may fetch the pinned upstream SDL2 revision recorded in
`cmake/Dependencies.cmake`. SDL2 is distributed under the zlib license; its
upstream source contains the authoritative license text.
