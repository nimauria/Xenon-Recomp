#pragma once

#include <cstddef>
#include <cstdint>
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

}  // namespace xenon::core
