#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace xenon::audio {

inline constexpr std::uint32_t kXboxSampleRate = 48000;
inline constexpr std::uint32_t kXboxRenderChannels = 6;
inline constexpr std::uint32_t kHostChannels = 2;
inline constexpr std::uint32_t kRenderFrameSamples = 256;
inline constexpr std::size_t kMaxQueuedBuffersPerVoice = 64;
// Source buffers are retained host-side until consumed. Bound retained PCM
// storage independently of buffer count so a game cannot hide timing bugs or
// exhaust host memory by queuing a few enormous buffers. This is storage
// backpressure, not a render-latency queue: samples are mixed directly into the
// next host callback.
inline constexpr std::size_t kMaxQueuedVoiceSampleBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kMaxRenderClients = 8;
inline constexpr std::size_t kMaxQueuedRenderFrames = 8;

using VoiceHandle = std::uint32_t;
using RenderClientHandle = std::uint32_t;

struct AudioFormat {
  std::uint32_t sample_rate{kXboxSampleRate};
  std::uint32_t channels{2};
};

struct VoiceRouting {
  float left{1.0f};
  float right{1.0f};
};

struct VoiceBuffer {
  std::vector<float> samples{};
  std::uint32_t channels{1};
  std::uint32_t loop_start_frame{};
  std::uint32_t loop_end_frame{};
  // 0 = no loop, 1..254 = finite repeats, 255 = infinite.
  std::uint8_t loop_count{};
  std::uint64_t tag{};
};

using VoiceCompletionCallback =
    std::function<void(VoiceHandle voice, std::uint64_t tag)>;

}  // namespace xenon::audio
