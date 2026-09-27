# Xenon Audio V1

## Purpose

Audio V1 is the common Xbox 360 audio translation layer owned by Xenon. A game
module supplies game code and game-specific data; it does **not** provide a
second audio engine. The production path is:

```
Xbox 360 audio data / xboxkrnl audio calls
        |
        v
Xenon AudioSystem + XMA context semantics
        |
        +--> XMA packet/frame decode (FFmpeg XMAFRAMES)
        +--> voice mixer / resampling / routing
        +--> Xbox 5.1 render-driver frame translation
        |
        v
bounded stereo float host stream
        |
        v
SDL2 native audio device (Windows / Linux)
```

The implementation is under `include/xenon/audio/` and `src/audio/`. Xbox
exports are registered through the central `core::ExportRegistry` by
`src/audio/exports.cpp`. `XenonSession` owns the subsystem when audio is enabled.

## Host dependencies

Audio V1 intentionally has no production `NullBackend` and does not report
success when native audio cannot be created.

* SDL2 provides the desktop host backend on Windows and Linux. CMake first
  uses an installed SDL2 and, for source builds with dependency fetching
  enabled, falls back to a pinned upstream SDL2 revision built statically.
* Windows x64 ships a Xenon-owned `libavcodec` + `libavutil` dependency under
  `third_party/xenon-ffmpeg/windows-x64`; users do not install Xenia or FFmpeg.
  Other platforms may provide a compatible FFmpeg through
  `XENON_AUDIO_FFMPEG_ROOT` or their system package manager.
* Every selected FFmpeg is compile-probed for `AV_CODEC_ID_XMAFRAMES`. Stock
  FFmpeg may expose XMA1/XMA2 container decoders without this raw hardware-frame
  interface, so incompatible builds fail at configure time rather than silently
  selecting the wrong semantic layer.

CMake enables Audio V1 by default. A developer may explicitly configure
`-DXENON_ENABLE_AUDIO=OFF` for dependency-isolation work, but a runtime built
that way will reject a session that requests audio instead of silently using a
fake backend.

SDL2 is zlib-licensed. The bundled Windows FFmpeg archives come from the
non-GPL Xenia-maintained FFmpeg configuration and retain FFmpeg's LGPL terms;
see `THIRD_PARTY_NOTICES.md` and `third_party/xenon-ffmpeg/README.md`.
Xbox/XMA semantics remain a clean Xenon implementation informed by public
behavior/reference projects; Xenon has no runtime dependency on Xenia.

## Render-driver path

The Xbox render-driver path supports up to eight clients. Each client owns at
most eight queued Xbox render frames, bounding queued latency to roughly 42.7 ms
at 48 kHz before the host callback itself.

A submitted render frame contains six sequential 256-sample big-endian float
channels in Xbox order:

1. front left
2. front right
3. center
4. LFE
5. back left
6. back right

The common stereo path ignores LFE and downmixes front, back, and center
channels. Partial host callbacks retain a per-client frame offset rather than
restarting the same Xbox frame.

`XAudioGetRenderDriverTic` exposes a 48 kHz consumed-sample clock. The clock
advances for every host frame, including injected underrun silence, so guest
A/V synchronization follows playback time rather than queue depth. Underruns
are separately counted by `XAudioGetUnderrunCount`.

Render-client callbacks receive a guest pointer to a four-byte wrapper that
contains the original callback argument, matching the Xbox render-driver ABI.
Callbacks are driven by bounded queue credits: registration grants one credit
per render-frame slot, and a credit is returned only when a 256-sample frame is
actually consumed. A callback that submits nothing therefore cannot be polled
forever by the host thread. Callback wrappers are reference-protected across
in-flight guest calls: unregister waits for an external in-flight callback,
while self-unregister defers wrapper destruction until the callback unwinds.
`XenonSession` invokes these callbacks with an independent `CpuState` and a
dedicated 128 KiB guest PPC stack. This avoids racing the main guest thread's
register state.

### Guest callback execution context (Gracemeria readiness pass, Part 4)

The audio render-driver callback runs on its own real guest thread identity,
not an isolated bare `CpuState`. `XenonSession::start_audio_guest_thread()`
(called once from `create_guest_process()`, after `kernel_process_`/
`loaded_xex_` exist) allocates a dedicated KPCR + static-TLS block for the
callback (`guest_thread_context.hpp`, the same mechanism the main thread
uses, but a fully independent instance) and creates a real
`kernel::KernelThread` whose entry (`run_audio_callback_thread()`) registers
itself with `kernel::ThreadManager::set_current_thread()` before running
`AudioSystem::run_guest_callback_pump_body()`. `AudioSystem::initialize()`'s
`auto_start_callback_pump` parameter (default `true`, used by
`AudioSystem`'s own standalone unit tests) is `false` for a real session -
the pump only starts once this real thread context exists, not at session-
init time.

Each `invoke_audio_callback()` call sets `gpr[13]` to this thread's own KPCR
(not the main thread's), so compiled guest code that reads `__declspec(thread)`
TLS, calls a real xboxkrnl export, or makes a genuine guest-to-guest `bl`
from inside the callback does so through the exact same production
mechanisms the main thread uses - no special callback-only dispatcher. Guest
callback threads are stopped (`AudioSystem::stop_guest_callback_pump()`) and
joined before `kernel_process_`/`memory_` are destroyed during shutdown, so
no callback worker is ever left running or leaked.

See `tests/core/audio_guest_callback_tests.cpp` for the end-to-end proof
(distinct KPCR from the main thread, a real guest-to-guest call, a real
xboxkrnl export call, and clean shutdown with an active render client).

## XMA contexts

Xenon owns the 320 hardware XMA contexts in one Memory V2 physical allocation.
The Xbox-visible context is 64 bytes and is encoded/decoded explicitly as
big-endian dwords rather than using host compiler bitfield layout.

Implemented state includes:

* input buffer 0/1 packet counts and valid flags
* current input buffer and bit read offset
* packet metadata
* output buffer block count, read/write offsets and valid flag
* sample-rate and mono/stereo selection
* loop start/end/count, loop subframe trim/skip and 256-byte decode quanta
* output-buffer padding/headroom semantics
* parser/decode error status plus their hardware error-set bits
* stop/interrupt flags retained in the guest-visible context; terminal completion is published once per enable sequence and carries the interrupt-request bit to the decoder completion sink
* asynchronous enable/disable/block state, including disable-vs-worker
  arbitration so a completing decode cannot accidentally re-enable a context
* decoded-sample accounting using lock-free publication for concurrent readers

Input/output addresses are translated through Memory V2. Decoder output and
asynchronous context progression use `AddressSpace::write_physical`, so XMA DMA
writes participate in reservation invalidation and Xenon coherency publication.
Guest API setters use the normal `MemoryPort` guest-write path.

The XMA MMIO aperture at `0x7FEA0000` is registered with Memory V2. The context-array address, rotating current/next context indices, and the ten 32-context kick/lock/clear register groups converge on the same decoder state used by xboxkrnl exports. This allows titles that access the XMA unit directly rather than only through helper exports to use the common Xenon implementation.

`XMASetLoopData` follows the XDK/Xenia ABI and reads its source as a full 64-byte `XMA_CONTEXT_DATA` hardware view. The compact 12-byte loop tail exists only inside `XMA_CONTEXT_INIT` and is not substituted for the standalone export ABI.

The worker uses FFmpeg's `XMAFRAMES` decoder. XMA1/XMA2 are not selected by
packet metadata; the common Xbox frame decoder is used because the hardware
packet/frame stream is the relevant interface at this layer.

The output ring uses 256-byte hardware blocks and never allocates an unbounded
host queue. `subframe_decode_count` is interpreted in those hardware blocks
(stereo uses two blocks per 128 samples), and output padding reserves headroom
at frame boundaries. Decoded frames can therefore be consumed over multiple
worker passes without overfilling the guest ring. Context work stops when the
ring cannot accept the next bounded quantum.

FFmpeg raw-frame output is realigned to Xbox sample numbering by carrying the
post-start-padding tail of one decoded frame into the head of the next. Loop
end trimming and loop-start subframe skipping are applied to the aligned Xbox
frame, not to the decoder's shifted intermediate output. Rolling input buffer
0/1 streams are gathered logically across their physical discontinuity, so a
compressed frame may span the two guest buffers without requiring contiguous
RAM.

Asynchronous decoder publication merges only XMA-engine-owned fields into a
fresh guest context before the Memory V2 external write. This avoids replacing
CPU-side buffer/read-offset updates with a stale 64-byte decoder snapshot.

## Voice mixer

`Mixer` is host-backend independent and supports:

* mono/stereo source buffers
* source sample-rate conversion to the host rate
* pitch
* independent left/right routing gain
* voice gain
* finite looping and `255` infinite looping
* bounded per-voice queues (64 buffers plus a 16 MiB retained-PCM ceiling)
* source-buffer completion callbacks
* additive mixing with the Xbox render-driver stream

Completion callbacks are dispatched after the mixer mutex is released, so a
callback can safely manipulate its own voice. A buffer that ends exactly on a
host callback boundary is completed in that same mixer call rather than being
deferred indefinitely. The source-buffer byte ceiling bounds retained content
memory; it is not used as host latency. Host playback latency remains governed
by the eight-frame render-driver queue and the 256-sample host callback.

## xboxkrnl exports

Audio V1 registers the central render-driver, speaker/category-volume, XMA
context, suspend, render-clock, ducking, and underrun exports used by the Xbox
audio path. The registered ordinal ranges include `0x1F2-0x200`,
`0x224-0x238`, `0x2D4`, and `0x34C-0x35A` for the implemented functions.

These handlers operate on `AudioSystem`/`XmaDecoder`; they do not contain
per-game behavior.

## Tests

`tests/audio/` contains:

* `mixer_tests.cpp` - mixing, routing, resampling/pitch, loops, bounded queues,
  and callback reentrancy/completion timing.
* `xma_tests.cpp` - packet headers, hardware-context round trips, context
  allocation/initialization, mono/stereo channel-mode semantics, ownership,
  decoded-sample/completion state, XMA MMIO kick/lock/clear behavior, and
  Memory V2 reservation invalidation from asynchronous context writes.
* `system_tests.cpp` - render-frame translation, partial host callbacks,
  render-driver timing through underruns, bounded callback credits, callback
  unregister lifetime, and additive generic voice mixing.
* `export_tests.cpp` - central export registration, render/XMA ABI dispatch,
  and the packed 64-byte `XMASetLoopData` contract.

The mixer test has no external native-audio dependency and can be built
standalone. The other Audio V1 tests require the same SDL2/FFmpeg development
packages as production Audio V1.

## Current validation boundary

The common Audio V1 code path is implemented. Windows x64 now carries a vetted
Xenon-owned FFmpeg/XMA dependency and CMake automatically validates the
`XMAFRAMES` API before enabling Audio. SDL2 can be resolved locally or fetched
from a pinned upstream revision. Mixer, XMA-context/MMIO, render-system, and
xboxkrnl export regression tests have passed in prior Audio V1 validation; the
export path has also passed ASan/UBSan.

Release qualification still requires a real SDL2 device plus a real compatible
FFmpeg build, and an actual Ace Combat 6 module/assets to prove title-level
initialize/stream/play behavior with compressed game data. The current Xenon
kernel also has no general APU interrupt-controller delivery API; therefore
`interrupt_when_done` is propagated correctly to the decoder completion sink,
but turning that request into a guest hardware interrupt is a kernel-level
prerequisite rather than an audio-local implementation. AC6 reference paths
examined for Audio V1 do not establish that this interrupt delivery is required.

Until real-title playback has been performed, Audio V1 should be treated as
code-complete for the common path but not title-qualified. Do not replace that
qualification with a null backend, fake-success exports, or a game-specific AC6
audio engine.
