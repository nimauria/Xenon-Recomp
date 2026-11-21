#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>

namespace xenon::core {

// Colour statistics of one presented frame (a small RGBA8 capture), and
// whether it differs from the frame before it the way a rendering fault does:
// a frame gone wholly black, white or green, or a sudden jump in brightness.
// A scene cut can trip the jump too; the anomaly log keeps the frames after
// it so a one-frame flash (which snaps back) can be told from a cut (which
// stays).
struct FrameColorStats {
  double mean_r{};
  double mean_g{};
  double mean_b{};
  double luminance{};         // 0..255
  double dark_fraction{};     // luminance under 16
  double bright_fraction{};   // luminance over 235
  double green_fraction{};    // green above both red and blue by 40
  bool valid{};
};

[[nodiscard]] FrameColorStats frame_color_stats(std::span<const std::byte> rgba8,
                                                std::uint32_t width, std::uint32_t height,
                                                std::uint32_t row_pitch);

// "black", "white", "green" or "brightness jump" for an anomalous change from
// `previous` to `current`; empty for an ordinary frame (or without a valid
// previous frame).
[[nodiscard]] std::string_view classify_frame_change(const FrameColorStats& previous,
                                                     const FrameColorStats& current);

// Decides when a detected anomaly arms the GPU frame capture (XENON_ANOMALY_
// CAPTURE=1): at most `max_count` captures in a run, and not within
// `cooldown_ms` of the previous one, so a run of black frames records one
// capture of the first rather than the title's whole bad stretch. Thread-safe;
// the clock is the caller's (milliseconds, any monotonic origin).
class AnomalyCaptureGate {
 public:
  struct Options {
    bool enabled{};
    std::uint32_t max_count{3};
    std::uint64_t cooldown_ms{10'000};
  };
  // XENON_ANOMALY_CAPTURE=1 (on), XENON_ANOMALY_CAPTURE_COUNT (default 3),
  // XENON_ANOMALY_CAPTURE_COOLDOWN_MS (default 10000).
  [[nodiscard]] static Options options_from_environment();

  explicit AnomalyCaptureGate(Options options = {}) : options_(options) {}
  [[nodiscard]] bool enabled() const noexcept { return options_.enabled; }
  [[nodiscard]] const Options& options() const noexcept { return options_; }
  // An anomaly was seen at `now_ms`: true when a capture should be armed for
  // it (and counted against the limits).
  [[nodiscard]] bool should_arm(std::uint64_t now_ms);
  [[nodiscard]] std::uint32_t armed_count() const;

 private:
  const Options options_;
  mutable std::mutex mutex_;
  std::uint32_t armed_{};
  bool have_last_{};
  std::uint64_t last_ms_{};
};

}  // namespace xenon::core
