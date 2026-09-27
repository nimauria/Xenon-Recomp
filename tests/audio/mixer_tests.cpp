#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "xenon/audio/mixer.hpp"

using namespace xenon::audio;

namespace {

bool near(float a, float b, float epsilon = 0.0001f) {
  return std::abs(a - b) <= epsilon;
}

void basic_mix_and_completion() {
  Mixer mixer;
  std::uint64_t completed_tag = 0;
  VoiceHandle voice = 0;
  voice = mixer.create_voice(
      AudioFormat{48000, 1},
      [&](VoiceHandle handle, std::uint64_t tag) {
        assert(handle == voice);
        completed_tag = tag;
      });
  assert(voice != 0);
  assert(mixer.set_volume(voice, 0.5f));
  assert(mixer.set_routing(voice, VoiceRouting{1.0f, 0.25f}));

  VoiceBuffer buffer{};
  buffer.samples = {1.0f, 0.5f, -1.0f, 0.0f};
  buffer.channels = 1;
  buffer.tag = 42;
  assert(mixer.submit(voice, std::move(buffer)));
  assert(mixer.start(voice));

  std::vector<float> output(8, 0.0f);
  mixer.mix(output, 48000);
  assert(near(output[0], 0.5f));
  assert(near(output[1], 0.125f));
  assert(near(output[2], 0.25f));
  assert(near(output[3], 0.0625f));
  assert(completed_tag == 42);
  assert(mixer.queued_buffers(voice) == 0);
}

void resampling_and_looping() {
  Mixer mixer;
  const auto voice = mixer.create_voice(AudioFormat{24000, 1});
  assert(voice != 0);
  assert(mixer.set_pitch(voice, 2.0f));

  VoiceBuffer buffer{};
  buffer.samples = {0.0f, 1.0f, 0.0f, -1.0f};
  buffer.channels = 1;
  buffer.loop_start_frame = 1;
  buffer.loop_end_frame = 4;
  buffer.loop_count = 2;
  assert(mixer.submit(voice, std::move(buffer)));
  assert(mixer.start(voice));

  // 24 kHz -> 48 kHz would normally step 0.5 source frames; pitch 2 restores
  // a 1-frame step. More output frames than source prove finite loops are
  // consumed rather than terminating on the first loop boundary.
  std::vector<float> output(18 * 2, 0.0f);
  mixer.mix(output, 48000);
  bool saw_positive = false;
  bool saw_negative = false;
  for (float sample : output) {
    saw_positive |= sample > 0.5f;
    saw_negative |= sample < -0.5f;
  }
  assert(saw_positive && saw_negative);
}

void loop_interpolation_wraps_to_loop_start() {
  Mixer mixer;
  const auto voice = mixer.create_voice(AudioFormat{48000, 1});
  assert(voice != 0);
  assert(mixer.set_pitch(voice, 0.5f));

  VoiceBuffer buffer{};
  // Frame 2 is deliberately far from the loop so an incorrect interpolation
  // across loop_end (1 -> 2) is obvious. Correct interpolation wraps 1 -> 0.
  buffer.samples = {0.0f, 1.0f, 10.0f};
  buffer.channels = 1;
  buffer.loop_start_frame = 0;
  buffer.loop_end_frame = 2;
  buffer.loop_count = 1;
  assert(mixer.submit(voice, std::move(buffer)));
  assert(mixer.start(voice));

  std::vector<float> output(4 * 2, 0.0f);
  mixer.mix(output, 48000);
  // Positions are 0, .5, 1, 1.5. The last interpolation must be 1 -> 0,
  // yielding 0.5 rather than the erroneous 5.5 from frame 1 -> frame 2.
  assert(near(output[6], 0.5f));
  assert(near(output[7], 0.5f));
}

void multichannel_downmix_and_format_validation() {
  Mixer mixer;
  const auto voice = mixer.create_voice(AudioFormat{48000, 6});
  assert(voice != 0);

  VoiceBuffer wrong{};
  wrong.samples = {1.0f, 1.0f};
  wrong.channels = 2;
  assert(!mixer.submit(voice, std::move(wrong)));

  VoiceBuffer surround{};
  surround.channels = 6;
  surround.samples = {1.0f, 0.5f, 0.25f, 1.0f, 0.25f, 0.0f};
  assert(mixer.submit(voice, std::move(surround)));
  assert(mixer.start(voice));
  std::vector<float> output(2, 0.0f);
  mixer.mix(output, 48000);
  assert(output[0] > output[1]);
  assert(output[0] < 1.0f);
}

void callback_is_reentrant() {
  Mixer mixer;
  VoiceHandle voice = 0;
  bool callback_called = false;
  voice = mixer.create_voice(
      AudioFormat{48000, 1},
      [&](VoiceHandle callback_voice, std::uint64_t) {
        callback_called = true;
        // Regression: completion used to run while the mixer mutex was held,
        // deadlocking callbacks that touched their own voice.
        assert(mixer.set_volume(callback_voice, 0.25f));
      });
  VoiceBuffer buffer{};
  buffer.samples = {0.25f};
  buffer.channels = 1;
  assert(mixer.submit(voice, std::move(buffer)));
  assert(mixer.start(voice));
  std::vector<float> output(2, 0.0f);
  mixer.mix(output, 48000);
  assert(callback_called);
}

void queue_is_bounded() {
  Mixer mixer;
  const auto voice = mixer.create_voice(AudioFormat{48000, 1});
  for (std::size_t i = 0; i < kMaxQueuedBuffersPerVoice; ++i) {
    VoiceBuffer buffer{};
    buffer.samples = {0.0f};
    buffer.channels = 1;
    assert(mixer.submit(voice, std::move(buffer)));
  }
  VoiceBuffer overflow{};
  overflow.samples = {0.0f};
  overflow.channels = 1;
  assert(!mixer.submit(voice, std::move(overflow)));
  assert(mixer.queued_buffers(voice) == kMaxQueuedBuffersPerVoice);
}

void queue_byte_budget_is_bounded_and_released() {
  Mixer mixer;
  const auto voice = mixer.create_voice(AudioFormat{48000, 1});
  assert(voice != 0);

  VoiceBuffer full{};
  full.channels = 1;
  full.samples.resize(kMaxQueuedVoiceSampleBytes / sizeof(float), 0.0f);
  assert(mixer.submit(voice, std::move(full)));
  assert(mixer.queued_sample_bytes(voice) == kMaxQueuedVoiceSampleBytes);

  VoiceBuffer overflow{};
  overflow.channels = 1;
  overflow.samples = {0.0f};
  assert(!mixer.submit(voice, std::move(overflow)));

  assert(mixer.stop(voice, true));
  assert(mixer.queued_sample_bytes(voice) == 0);

  VoiceBuffer short_buffer{};
  short_buffer.channels = 1;
  short_buffer.samples = {0.0f, 0.0f};
  assert(mixer.submit(voice, std::move(short_buffer)));
  assert(mixer.queued_sample_bytes(voice) == 2 * sizeof(float));
  assert(mixer.start(voice));
  std::vector<float> output(4, 0.0f);
  mixer.mix(output, 48000);
  assert(mixer.queued_buffers(voice) == 0);
  assert(mixer.queued_sample_bytes(voice) == 0);
}

}  // namespace

int main() {
  basic_mix_and_completion();
  resampling_and_looping();
  loop_interpolation_wraps_to_loop_start();
  multichannel_downmix_and_format_validation();
  callback_is_reentrant();
  queue_is_bounded();
  queue_byte_budget_is_bounded_and_released();
  std::cout << "Audio mixer tests passed\n";
  return 0;
}
