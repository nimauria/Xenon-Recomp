# Xenon Audio V1 drop-in

This archive contains the complete Audio V1 source tree produced from the supplied Xenon-Recomp repository.

## Direct overlay

The archive uses repository-relative paths. Copy/overlay the contents into the Xenon repository root. The included `CMakeLists.txt`, `include/xenon/core/session.hpp`, `src/core/session.cpp`, and `src/core/export_registry.cpp` are the integration versions matching the supplied repository snapshot.

If the main repository has moved on since that snapshot, copy the new audio files/directories first and apply `AUDIO_V1_INTEGRATION.patch` manually instead of overwriting newer integration files.

## Required native dependencies

- SDL2 development headers/libraries.
- A compatible FFmpeg build exposing `AV_CODEC_ID_XMAFRAMES` (the Xenia-compatible raw XMA frame decoder API). Point `XENON_AUDIO_FFMPEG_ROOT` at that build if it is not installed globally.

No fake SDL/FFmpeg test shims are included in this archive.

## Validation performed

- `xenon_audio` CMake target compiled against a test Xenia-compatible FFmpeg ABI and SDL backend shim.
- mixer regression tests: pass.
- XMA context/MMIO regression tests: pass.
- render-system regression tests: pass.
- xboxkrnl audio export regression tests: pass, including the 64-byte `XMASetLoopData` ABI.
- export path ASan/UBSan run: pass.
- production Audio V1 stub sweep: clean.
- `git diff --check` on the Audio/integration files: clean.

The test dependency shims used only for build/control-path validation are not included.

## Remaining external qualification

The common Audio V1 implementation is code-complete for the targeted path, but real Ace Combat 6 playback still needs to be run with the actual AC6 module/assets and a real Xenia-compatible FFmpeg build/audio device. Xenon's current kernel tree also does not expose a general APU interrupt-controller delivery API; XMA completion propagates `interrupt_when_done` to the decoder completion sink so the kernel can wire that signal when such an interrupt route exists.
