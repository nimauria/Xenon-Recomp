#include "xenon/audio/mixer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace xenon::audio {
namespace {

[[nodiscard]] float clamp_gain(float value) noexcept {
  return std::clamp(value, 0.0f, 16.0f);
}

[[nodiscard]] std::uint32_t frame_count(const VoiceBuffer& buffer) noexcept {
  if (!buffer.channels) return 0;
  return static_cast<std::uint32_t>(buffer.samples.size() / buffer.channels);
}

}  // namespace

VoiceHandle Mixer::create_voice(AudioFormat format,
                                VoiceCompletionCallback completion) {
  if (!format.sample_rate || !format.channels) return 0;
  std::lock_guard lock(mutex_);
  VoiceHandle handle = next_handle_++;
  if (!handle) handle = next_handle_++;
  Voice voice{};
  voice.format = format;
  voice.completion = std::move(completion);
  voices_.emplace(handle, std::move(voice));
  return handle;
}

bool Mixer::destroy_voice(VoiceHandle handle) {
  std::lock_guard lock(mutex_);
  return voices_.erase(handle) != 0;
}

bool Mixer::submit(VoiceHandle handle, VoiceBuffer buffer) {
  if (!buffer.channels || buffer.samples.empty() ||
      buffer.samples.size() % buffer.channels != 0) {
    return false;
  }
  const auto frames = frame_count(buffer);
  if (buffer.loop_count) {
    if (buffer.loop_end_frame == 0) buffer.loop_end_frame = frames;
    if (buffer.loop_start_frame >= buffer.loop_end_frame ||
        buffer.loop_end_frame > frames) {
      return false;
    }
  }

  const auto sample_bytes = buffer.samples.size() * sizeof(float);
  if (sample_bytes > kMaxQueuedVoiceSampleBytes) return false;

  std::lock_guard lock(mutex_);
  const auto it = voices_.find(handle);
  if (it == voices_.end() || buffer.channels != it->second.format.channels ||
      it->second.queue.size() >= kMaxQueuedBuffersPerVoice ||
      it->second.queued_sample_bytes > kMaxQueuedVoiceSampleBytes - sample_bytes) {
    return false;
  }
  QueuedBuffer queued{};
  queued.loops_remaining = buffer.loop_count;
  queued.buffer = std::move(buffer);
  it->second.queued_sample_bytes += sample_bytes;
  it->second.queue.push_back(std::move(queued));
  return true;
}

bool Mixer::start(VoiceHandle handle) {
  std::lock_guard lock(mutex_);
  const auto it = voices_.find(handle);
  if (it == voices_.end()) return false;
  it->second.running = true;
  return true;
}

bool Mixer::stop(VoiceHandle handle, bool flush) {
  std::lock_guard lock(mutex_);
  const auto it = voices_.find(handle);
  if (it == voices_.end()) return false;
  it->second.running = false;
  if (flush) {
    it->second.queue.clear();
    it->second.queued_sample_bytes = 0;
  }
  return true;
}

bool Mixer::set_volume(VoiceHandle handle, float volume) {
  if (!std::isfinite(volume)) return false;
  std::lock_guard lock(mutex_);
  const auto it = voices_.find(handle);
  if (it == voices_.end()) return false;
  it->second.volume = clamp_gain(volume);
  return true;
}

bool Mixer::set_pitch(VoiceHandle handle, float pitch) {
  if (!std::isfinite(pitch) || pitch <= 0.0f || pitch > 16.0f) return false;
  std::lock_guard lock(mutex_);
  const auto it = voices_.find(handle);
  if (it == voices_.end()) return false;
  it->second.pitch = pitch;
  return true;
}

bool Mixer::set_routing(VoiceHandle handle, VoiceRouting routing) {
  if (!std::isfinite(routing.left) || !std::isfinite(routing.right)) return false;
  std::lock_guard lock(mutex_);
  const auto it = voices_.find(handle);
  if (it == voices_.end()) return false;
  routing.left = clamp_gain(routing.left);
  routing.right = clamp_gain(routing.right);
  it->second.routing = routing;
  return true;
}

std::size_t Mixer::queued_buffers(VoiceHandle handle) const {
  std::lock_guard lock(mutex_);
  const auto it = voices_.find(handle);
  return it == voices_.end() ? 0 : it->second.queue.size();
}

std::size_t Mixer::queued_sample_bytes(VoiceHandle handle) const {
  std::lock_guard lock(mutex_);
  const auto it = voices_.find(handle);
  return it == voices_.end() ? 0 : it->second.queued_sample_bytes;
}

std::pair<float, float> Mixer::sample_stereo(const QueuedBuffer& queued,
                                             std::uint32_t frame_index) {
  const auto& buffer = queued.buffer;
  const auto frames = frame_count(buffer);
  if (!frames) return {};
  frame_index = std::min(frame_index, frames - 1u);
  const auto base = static_cast<std::size_t>(frame_index) * buffer.channels;
  if (buffer.channels == 1) {
    return {buffer.samples[base], buffer.samples[base]};
  }
  if (buffer.channels == 2) {
    return {buffer.samples[base], buffer.samples[base + 1]};
  }

  // Preserve Xbox's common 5.1 ordering when a voice carries six channels:
  // FL, FR, C, LFE, BL, BR. LFE is intentionally omitted from the stereo
  // downmix, matching the render-driver path. Other multichannel layouts keep
  // the first two channels as front L/R and fold remaining channels equally.
  if (buffer.channels == 6) {
    const float fl = buffer.samples[base + 0];
    const float fr = buffer.samples[base + 1];
    const float center = buffer.samples[base + 2];
    const float back_left = buffer.samples[base + 4];
    const float back_right = buffer.samples[base + 5];
    return {(fl + back_left + center * 0.5f) * (1.0f / 2.5f),
            (fr + back_right + center * 0.5f) * (1.0f / 2.5f)};
  }

  float left = buffer.samples[base];
  float right = buffer.samples[base + 1];
  float folded = 0.0f;
  for (std::uint32_t channel = 2; channel < buffer.channels; ++channel) {
    folded += buffer.samples[base + channel];
  }
  folded *= 0.5f / static_cast<float>(buffer.channels - 2u);
  return {left + folded, right + folded};
}

bool Mixer::normalize_position(QueuedBuffer& queued) {
  const auto frames = frame_count(queued.buffer);
  if (!frames) return false;

  if (queued.loops_remaining != 0) {
    const auto loop_start = queued.buffer.loop_start_frame;
    const auto loop_end = queued.buffer.loop_end_frame ?
        queued.buffer.loop_end_frame : frames;
    const auto loop_length = loop_end > loop_start ? loop_end - loop_start : 0;
    if (!loop_length) return false;

    while (queued.frame_position >= static_cast<double>(loop_end) &&
           queued.loops_remaining != 0) {
      queued.frame_position = static_cast<double>(loop_start) +
          (queued.frame_position - static_cast<double>(loop_end));
      if (queued.loops_remaining != 255) --queued.loops_remaining;
    }
  }

  return queued.frame_position < static_cast<double>(frames);
}

void Mixer::mix(std::span<float> destination, std::uint32_t output_sample_rate) {
  if (!output_sample_rate || destination.size() < 2) return;
  const auto output_frames = destination.size() / 2;
  std::vector<PendingCompletion> completions;

  {
    std::lock_guard lock(mutex_);
    for (auto& [handle, voice] : voices_) {
      if (!voice.running) continue;
      const double step =
          (static_cast<double>(voice.format.sample_rate) /
           static_cast<double>(output_sample_rate)) * voice.pitch;

      for (std::size_t out_frame = 0; out_frame < output_frames; ++out_frame) {
        while (!voice.queue.empty() && !normalize_position(voice.queue.front())) {
          auto finished = std::move(voice.queue.front());
          const auto finished_bytes = finished.buffer.samples.size() * sizeof(float);
          voice.queued_sample_bytes =
              finished_bytes > voice.queued_sample_bytes
                  ? 0
                  : voice.queued_sample_bytes - finished_bytes;
          voice.queue.pop_front();
          if (voice.completion) {
            completions.push_back({voice.completion, handle, finished.buffer.tag});
          }
        }
        if (voice.queue.empty()) break;

        auto& queued = voice.queue.front();
        const auto frames = frame_count(queued.buffer);
        if (!frames) continue;
        const auto index0 = static_cast<std::uint32_t>(queued.frame_position);
        auto index1 = index0 + 1u;
        if (queued.loops_remaining != 0) {
          const auto loop_end = queued.buffer.loop_end_frame
                                    ? queued.buffer.loop_end_frame
                                    : frames;
          if (index1 >= loop_end) index1 = queued.buffer.loop_start_frame;
        } else if (index1 >= frames) {
          index1 = index0;
        }
        const float frac = static_cast<float>(
            queued.frame_position - static_cast<double>(index0));
        const auto a = sample_stereo(queued, index0);
        const auto b = sample_stereo(queued, index1);
        const float left = a.first + (b.first - a.first) * frac;
        const float right = a.second + (b.second - a.second) * frac;

        destination[out_frame * 2] += left * voice.volume * voice.routing.left;
        destination[out_frame * 2 + 1] += right * voice.volume * voice.routing.right;
        queued.frame_position += step;
      }

      // If the final output sample consumed a source buffer exactly at this
      // host callback boundary, publish its completion now rather than waiting
      // for a later callback that may never arrive (for example when a title
      // stops the voice immediately after the buffer-end notification).
      while (!voice.queue.empty() && !normalize_position(voice.queue.front())) {
        auto finished = std::move(voice.queue.front());
        const auto finished_bytes = finished.buffer.samples.size() * sizeof(float);
        voice.queued_sample_bytes =
            finished_bytes > voice.queued_sample_bytes
                ? 0
                : voice.queued_sample_bytes - finished_bytes;
        voice.queue.pop_front();
        if (voice.completion) {
          completions.push_back({voice.completion, handle, finished.buffer.tag});
        }
      }
    }
  }

  // User callbacks may submit/stop/destroy voices, so never invoke them while
  // holding the mixer mutex. This is a production deadlock regression guard.
  for (auto& completion : completions) {
    completion.callback(completion.handle, completion.tag);
  }
}

}  // namespace xenon::audio
