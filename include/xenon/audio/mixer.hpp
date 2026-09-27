#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenon/audio/types.hpp"

namespace xenon::audio {

class Mixer final {
 public:
  Mixer() = default;

  [[nodiscard]] VoiceHandle create_voice(
      AudioFormat format,
      VoiceCompletionCallback completion = {});
  [[nodiscard]] bool destroy_voice(VoiceHandle handle);
  [[nodiscard]] bool submit(VoiceHandle handle, VoiceBuffer buffer);
  [[nodiscard]] bool start(VoiceHandle handle);
  [[nodiscard]] bool stop(VoiceHandle handle, bool flush = false);
  [[nodiscard]] bool set_volume(VoiceHandle handle, float volume);
  [[nodiscard]] bool set_pitch(VoiceHandle handle, float pitch);
  [[nodiscard]] bool set_routing(VoiceHandle handle, VoiceRouting routing);
  [[nodiscard]] std::size_t queued_buffers(VoiceHandle handle) const;
  [[nodiscard]] std::size_t queued_sample_bytes(VoiceHandle handle) const;

  // Adds mixed stereo samples into destination. The caller controls whether
  // destination is pre-cleared, allowing render-driver and voice paths to be
  // accumulated without one silently replacing the other.
  void mix(std::span<float> destination, std::uint32_t output_sample_rate = 48000);

 private:
  struct QueuedBuffer {
    VoiceBuffer buffer{};
    double frame_position{};
    std::uint8_t loops_remaining{};
  };

  struct Voice {
    AudioFormat format{};
    VoiceRouting routing{};
    float volume{1.0f};
    float pitch{1.0f};
    bool running{};
    VoiceCompletionCallback completion{};
    std::deque<QueuedBuffer> queue{};
    std::size_t queued_sample_bytes{};
  };

  struct PendingCompletion {
    VoiceCompletionCallback callback{};
    VoiceHandle handle{};
    std::uint64_t tag{};
  };

  [[nodiscard]] static std::pair<float, float> sample_stereo(
      const QueuedBuffer& queued, std::uint32_t frame_index);
  static bool normalize_position(QueuedBuffer& queued);

  mutable std::mutex mutex_{};
  std::unordered_map<VoiceHandle, Voice> voices_{};
  VoiceHandle next_handle_{1};
};

}  // namespace xenon::audio
