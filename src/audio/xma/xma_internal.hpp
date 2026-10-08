#pragma once

// Private to the XMA decoder: runtime state and APU register layout shared by
// decoder.cpp, decode.cpp and mmio.cpp.

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <mutex>

#include "xenon/audio/xma.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#if defined(XENON_AUDIO_FFMPEG_HAS_CH_LAYOUT)
#include <libavutil/channel_layout.h>
#endif
#include <libavutil/samplefmt.h>
}

namespace xenon::audio {

// XMA register indices from the Xbox 360 APU register file, converted to byte
// offsets inside the 0x7FEA0000 MMIO aperture. Context groups contain ten
// dwords, each controlling 32 of the 320 hardware contexts.
inline constexpr std::uint32_t kRegContextArrayAddress = 0x0600u * 4u;
inline constexpr std::uint32_t kRegCurrentContextIndex = 0x0606u * 4u;
inline constexpr std::uint32_t kRegNextContextIndex = 0x0607u * 4u;
inline constexpr std::uint32_t kRegContextKickBase = 0x0650u * 4u;
inline constexpr std::uint32_t kRegContextLockBase = 0x0690u * 4u;
inline constexpr std::uint32_t kRegContextClearBase = 0x06A0u * 4u;
inline constexpr std::uint32_t kContextRegisterGroupBytes = 10u * 4u;

struct XmaDecoder::Runtime {
  static constexpr std::size_t kMaxFrameBytes =
      kXmaSamplesPerFrame * 2u * sizeof(std::int16_t);
  static constexpr std::size_t kDecoderStartPaddingSamples = 192;

  mutable std::mutex mutex{};
  std::condition_variable cv{};
  bool enabled{};
  bool busy{};
  // Set by XMADisableContext while a decode is in flight. The worker must not
  // re-arm the context when finishing that decode.
  bool disable_requested{};
  std::atomic<std::uint64_t> decoded_samples{0};
  const AVCodec* codec{};
  AVCodecContext* codec_context{};
  AVPacket* packet{};
  AVFrame* frame{};
  int configured_sample_rate{};
  int configured_channels{};

  // FFmpeg's XMAFRAMES output is shifted by the codec's start padding. Keep
  // the tail of one decoded frame and combine it with the head of the next so
  // guest-visible PCM is aligned to Xbox frame/sample numbering.
  std::array<std::byte, kMaxFrameBytes> carry_tail{};
  std::size_t carry_tail_bytes{};
  bool carry_valid{};
  std::uint8_t carry_output_limit_blocks{};
  std::uint8_t carry_start_skip_blocks{};
  bool loop_start_skip_pending{};
  bool completion_notified{};

  // One assembled Xbox frame may be consumed over several hardware kicks
  // because subframe_decode_count is expressed in 256-byte output blocks.
  std::array<std::byte, kMaxFrameBytes> pending_pcm{};
  std::size_t pending_bytes{};
  std::size_t pending_offset{};

  void reset_stream() noexcept {
    enabled = false;
    disable_requested = false;
    decoded_samples.store(0, std::memory_order_relaxed);
    configured_sample_rate = 0;
    configured_channels = 0;
    if (codec_context) avcodec_free_context(&codec_context);
    if (packet) av_packet_unref(packet);
    if (frame) av_frame_unref(frame);
    carry_tail.fill(std::byte{0});
    carry_tail_bytes = 0;
    carry_valid = false;
    carry_output_limit_blocks = 0;
    carry_start_skip_blocks = 0;
    loop_start_skip_pending = false;
    completion_notified = false;
    pending_pcm.fill(std::byte{0});
    pending_bytes = 0;
    pending_offset = 0;
  }

  ~Runtime() {
    if (packet) av_packet_free(&packet);
    if (frame) av_frame_free(&frame);
    if (codec_context) avcodec_free_context(&codec_context);
  }
};

struct XmaDecoder::MmioBridge {
  std::mutex mutex{};
  XmaDecoder* owner{};
};

}  // namespace xenon::audio
