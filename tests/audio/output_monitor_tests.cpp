// OutputMeter and OutputRecorder: what reached the host audio device.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#include "xenon/audio/output_monitor.hpp"

using namespace xenon::audio;

namespace {

std::vector<float> tone(std::size_t frames, float amplitude) {
  std::vector<float> samples(frames * 2u);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const float value = amplitude * std::sin(0.05f * float(frame));
    samples[frame * 2u] = value;
    samples[frame * 2u + 1u] = -value;
  }
  return samples;
}

std::uint32_t le32(const std::vector<unsigned char>& bytes, std::size_t at) {
  return std::uint32_t(bytes[at]) | std::uint32_t(bytes[at + 1]) << 8 |
         std::uint32_t(bytes[at + 2]) << 16 | std::uint32_t(bytes[at + 3]) << 24;
}

// Silence is counted as delivered but not audible; a tone is audible and
// sets the peak.
void test_meter_separates_silence_from_sound() {
  OutputMeter meter;
  const std::vector<float> silence(256u * 2u, 0.0f);
  meter.observe(silence);
  auto snapshot = meter.snapshot();
  assert(snapshot.frames == 256 && snapshot.audible_frames == 0);
  assert(snapshot.peak == 0.0f && snapshot.recent_rms == 0.0f);

  meter.observe(tone(256, 0.5f));
  snapshot = meter.snapshot();
  assert(snapshot.frames == 512 && snapshot.callbacks == 2);
  assert(snapshot.audible_frames > 200);  // all but the zero crossings
  assert(snapshot.peak > 0.45f && snapshot.peak <= 0.5f);
  assert(snapshot.recent_rms > 0.3f && snapshot.recent_rms < 0.4f);  // 0.5 / sqrt(2)

  // The peak is kept; the RMS is the latest callback's.
  meter.observe(silence);
  snapshot = meter.snapshot();
  assert(snapshot.peak > 0.45f && snapshot.recent_rms == 0.0f);
  assert(OutputMeter::describe(snapshot, 48000).find("callbacks") != std::string::npos);
}

// The recording is a valid 16-bit stereo WAV holding exactly what was pushed.
void test_recorder_writes_the_pushed_frames_as_wav() {
  const auto path = std::filesystem::temp_directory_path() / "xenon_output_monitor_test.wav";
  std::filesystem::remove(path);
  OutputRecorder recorder;
  std::string error;
  assert(recorder.start(path, 48000, 1, &error));
  assert(recorder.recording());
  const auto samples = tone(1000, 0.25f);
  recorder.push(samples);
  recorder.push(samples);
  recorder.stop();
  assert(!recorder.recording());
  assert(recorder.frames_written() == 2000 && recorder.frames_dropped() == 0);

  std::ifstream file(path, std::ios::binary);
  const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), {}};
  file.close();
  assert(bytes.size() == 44u + 2000u * 4u);
  assert(std::string(bytes.begin(), bytes.begin() + 4) == "RIFF");
  assert(std::string(bytes.begin() + 8, bytes.begin() + 16) == "WAVEfmt ");
  assert(le32(bytes, 24) == 48000);    // sample rate
  assert(le32(bytes, 40) == 2000u * 4u);  // data bytes
  // First frame after a sample in: sin(0.05) * 0.25 * 32767, then its negation.
  const auto left = std::int16_t(bytes[44 + 4] | bytes[45 + 4] << 8);
  const auto right = std::int16_t(bytes[46 + 4] | bytes[47 + 4] << 8);
  const auto expected = std::lround(std::sin(0.05f) * 0.25f * 32767.0f);
  assert(left == expected && right == -expected);
  std::filesystem::remove(path);
}

// A push that does not fit the buffer is dropped and counted; the device
// thread never waits for the writer.
void test_recorder_drops_rather_than_blocks_when_full() {
  const auto path = std::filesystem::temp_directory_path() / "xenon_output_monitor_full.wav";
  OutputRecorder recorder;
  assert(recorder.start(path, 1000, 1));  // room for 1000 frames
  recorder.push(tone(5000, 0.1f));
  recorder.stop();
  assert(recorder.frames_dropped() == 5000 && recorder.frames_written() == 0);
  std::filesystem::remove(path);
}

}  // namespace

int main() {
  test_meter_separates_silence_from_sound();
  test_recorder_writes_the_pushed_frames_as_wav();
  test_recorder_drops_rather_than_blocks_when_full();
  std::cout << "All output monitor tests passed!\n";
  return 0;
}
