#pragma once

// What actually reached the host audio device: levels for the stop report,
// and an optional WAV recording of the runtime's own output for development
// (XENON_AUDIO_CAPTURE=<path>). Both are fed from the device callback, so
// neither blocks, allocates or does I/O there: the meter is atomics, and the
// recorder copies into a preallocated ring that a writer thread drains.

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace xenon::audio {

class OutputMeter {
 public:
  // A sample whose magnitude is at or below this is silence.
  static constexpr float kSilence = 1.0f / 32768.0f;

  // Device thread: the final output of one callback, interleaved stereo.
  void observe(std::span<const float> interleaved_stereo) noexcept;

  struct Snapshot {
    std::uint64_t frames{};          // stereo frames delivered to the device
    std::uint64_t audible_frames{};  // frames with a sample above kSilence
    float peak{};                    // largest magnitude so far
    float recent_rms{};              // RMS of the most recent callback
    std::uint64_t callbacks{};
  };
  [[nodiscard]] Snapshot snapshot() const noexcept;
  [[nodiscard]] static std::string describe(const Snapshot& snapshot, std::uint32_t sample_rate);

 private:
  std::atomic<std::uint64_t> frames_{0};
  std::atomic<std::uint64_t> audible_frames_{0};
  std::atomic<float> peak_{0.0f};
  std::atomic<float> recent_rms_{0.0f};
  std::atomic<std::uint64_t> callbacks_{0};
};

// Records the device output to a 16-bit stereo PCM WAV file.
class OutputRecorder {
 public:
  OutputRecorder() = default;
  ~OutputRecorder();
  OutputRecorder(const OutputRecorder&) = delete;
  OutputRecorder& operator=(const OutputRecorder&) = delete;

  // Opens `path` and starts the writer. `buffer_seconds` of audio can be held
  // between drains; anything beyond is dropped and counted, never waited for.
  [[nodiscard]] bool start(const std::filesystem::path& path, std::uint32_t sample_rate,
                           std::uint32_t buffer_seconds = 4, std::string* error = nullptr);
  // Device thread: copies one callback's output into the ring.
  void push(std::span<const float> interleaved_stereo) noexcept;
  // Drains what is buffered, completes the WAV header and closes the file.
  void stop();

  [[nodiscard]] bool recording() const noexcept { return running_.load(); }
  [[nodiscard]] std::uint64_t frames_written() const noexcept { return frames_written_.load(); }
  [[nodiscard]] std::uint64_t frames_dropped() const noexcept { return frames_dropped_.load(); }

 private:
  void writer_loop();
  bool drain();

  std::vector<float> ring_;
  std::atomic<std::size_t> write_position_{0};  // samples, monotonically increasing
  std::atomic<std::size_t> read_position_{0};
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> frames_written_{0};
  std::atomic<std::uint64_t> frames_dropped_{0};
  std::uint32_t sample_rate_{0};
  std::FILE* file_{nullptr};
  std::thread writer_;
  std::mutex wake_mutex_;
  std::condition_variable wake_;
};

}  // namespace xenon::audio
