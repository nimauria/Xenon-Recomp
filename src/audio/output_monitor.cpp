#include "xenon/audio/output_monitor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace xenon::audio {

void OutputMeter::observe(std::span<const float> interleaved_stereo) noexcept {
  const auto frames = interleaved_stereo.size() / 2u;
  if (!frames) return;
  std::uint64_t audible = 0;
  float peak = 0.0f;
  double sum_squares = 0.0;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const float left = std::abs(interleaved_stereo[frame * 2u]);
    const float right = std::abs(interleaved_stereo[frame * 2u + 1u]);
    const float loudest = (std::max)(left, right);
    if (loudest > kSilence) ++audible;
    peak = (std::max)(peak, loudest);
    sum_squares += double(left) * left + double(right) * right;
  }
  frames_.fetch_add(frames, std::memory_order_relaxed);
  audible_frames_.fetch_add(audible, std::memory_order_relaxed);
  recent_rms_.store(static_cast<float>(std::sqrt(sum_squares / double(frames * 2u))),
                    std::memory_order_relaxed);
  // Only the device thread writes the peak.
  if (peak > peak_.load(std::memory_order_relaxed)) peak_.store(peak, std::memory_order_relaxed);
  callbacks_.fetch_add(1, std::memory_order_relaxed);
}

OutputMeter::Snapshot OutputMeter::snapshot() const noexcept {
  Snapshot snapshot{};
  snapshot.frames = frames_.load(std::memory_order_relaxed);
  snapshot.audible_frames = audible_frames_.load(std::memory_order_relaxed);
  snapshot.peak = peak_.load(std::memory_order_relaxed);
  snapshot.recent_rms = recent_rms_.load(std::memory_order_relaxed);
  snapshot.callbacks = callbacks_.load(std::memory_order_relaxed);
  return snapshot;
}

std::string OutputMeter::describe(const Snapshot& snapshot, std::uint32_t sample_rate) {
  char text[200];
  const double seconds = sample_rate ? double(snapshot.frames) / sample_rate : 0.0;
  const double audible_seconds = sample_rate ? double(snapshot.audible_frames) / sample_rate : 0.0;
  std::snprintf(text, sizeof(text),
                "Audio device output: %.1f s delivered in %llu callbacks, %.1f s audible, "
                "peak %.4f, last callback RMS %.4f",
                seconds, static_cast<unsigned long long>(snapshot.callbacks), audible_seconds,
                snapshot.peak, snapshot.recent_rms);
  return text;
}

namespace {

void put16(std::FILE* file, std::uint16_t value) {
  const std::array<unsigned char, 2> bytes{static_cast<unsigned char>(value),
                                           static_cast<unsigned char>(value >> 8)};
  std::fwrite(bytes.data(), 1, bytes.size(), file);
}

void put32(std::FILE* file, std::uint32_t value) {
  const std::array<unsigned char, 4> bytes{
      static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8),
      static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 24)};
  std::fwrite(bytes.data(), 1, bytes.size(), file);
}

void write_wav_header(std::FILE* file, std::uint32_t sample_rate, std::uint32_t data_bytes) {
  std::fseek(file, 0, SEEK_SET);
  std::fwrite("RIFF", 1, 4, file);
  put32(file, 36u + data_bytes);
  std::fwrite("WAVEfmt ", 1, 8, file);
  put32(file, 16u);              // fmt chunk size
  put16(file, 1u);               // PCM
  put16(file, 2u);               // stereo
  put32(file, sample_rate);
  put32(file, sample_rate * 4u); // byte rate
  put16(file, 4u);               // block align
  put16(file, 16u);              // bits per sample
  std::fwrite("data", 1, 4, file);
  put32(file, data_bytes);
}

}  // namespace

OutputRecorder::~OutputRecorder() { stop(); }

bool OutputRecorder::start(const std::filesystem::path& path, std::uint32_t sample_rate,
                           std::uint32_t buffer_seconds, std::string* error) {
  stop();
  if (!sample_rate || !buffer_seconds) {
    if (error) *error = "invalid audio capture configuration";
    return false;
  }
#if defined(_WIN32)
  file_ = _wfopen(path.c_str(), L"wb");
#else
  file_ = std::fopen(path.c_str(), "wb");
#endif
  if (!file_) {
    if (error) *error = "cannot open audio capture file " + path.string();
    return false;
  }
  sample_rate_ = sample_rate;
  ring_.assign(std::size_t(sample_rate) * 2u * buffer_seconds, 0.0f);
  write_position_ = 0;
  read_position_ = 0;
  frames_written_ = 0;
  frames_dropped_ = 0;
  write_wav_header(file_, sample_rate_, 0u);
  running_ = true;
  writer_ = std::thread(&OutputRecorder::writer_loop, this);
  return true;
}

void OutputRecorder::push(std::span<const float> interleaved_stereo) noexcept {
  if (!running_.load(std::memory_order_acquire) || ring_.empty()) return;
  const auto write = write_position_.load(std::memory_order_relaxed);
  const auto read = read_position_.load(std::memory_order_acquire);
  const auto free = ring_.size() - (write - read);
  if (interleaved_stereo.size() > free) {
    frames_dropped_.fetch_add(interleaved_stereo.size() / 2u, std::memory_order_relaxed);
    return;
  }
  for (std::size_t i = 0; i < interleaved_stereo.size(); ++i) {
    ring_[(write + i) % ring_.size()] = interleaved_stereo[i];
  }
  write_position_.store(write + interleaved_stereo.size(), std::memory_order_release);
}

bool OutputRecorder::drain() {
  const auto write = write_position_.load(std::memory_order_acquire);
  auto read = read_position_.load(std::memory_order_relaxed);
  if (write == read) return false;
  std::vector<std::int16_t> pcm;
  pcm.reserve(write - read);
  for (; read != write; ++read) {
    const float sample = std::clamp(ring_[read % ring_.size()], -1.0f, 1.0f);
    pcm.push_back(static_cast<std::int16_t>(std::lround(sample * 32767.0f)));
  }
  read_position_.store(read, std::memory_order_release);
  for (const auto sample : pcm) put16(file_, static_cast<std::uint16_t>(sample));
  frames_written_.fetch_add(pcm.size() / 2u, std::memory_order_relaxed);
  return true;
}

void OutputRecorder::writer_loop() {
  while (running_.load()) {
    {
      std::unique_lock lock(wake_mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(100), [&] { return !running_.load(); });
    }
    drain();
  }
}

void OutputRecorder::stop() {
  if (!file_) return;
  running_ = false;
  wake_.notify_all();
  if (writer_.joinable()) writer_.join();
  drain();
  const auto data_bytes = static_cast<std::uint32_t>(
      (std::min<std::uint64_t>)(frames_written_.load() * 4u, 0xFFFFFFDBu));
  write_wav_header(file_, sample_rate_, data_bytes);
  std::fclose(file_);
  file_ = nullptr;
}

}  // namespace xenon::audio
