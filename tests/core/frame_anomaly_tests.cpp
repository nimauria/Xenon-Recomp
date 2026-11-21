// Frame anomaly classification: a frame that goes wholly black, white or
// green, or jumps in brightness, is flagged against the frame before it;
// ordinary frames and frames that stay dark (a fade that holds) are not.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "xenon/core/frame_anomaly.hpp"

using xenon::core::classify_frame_change;
using xenon::core::frame_color_stats;
using xenon::core::FrameColorStats;

namespace {

constexpr std::uint32_t kW = 16, kH = 9, kPitch = kW * 4u + 8u;  // padded rows

FrameColorStats solid(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                      std::uint32_t differing_pixels = 0) {
  std::vector<std::byte> pixels(static_cast<std::size_t>(kPitch) * kH, std::byte{0xEE});
  std::uint32_t n = 0;
  for (std::uint32_t y = 0; y < kH; ++y)
    for (std::uint32_t x = 0; x < kW; ++x, ++n) {
      auto* p = pixels.data() + y * kPitch + x * 4u;
      const bool other = n < differing_pixels;
      p[0] = std::byte{other ? std::uint8_t(128) : r};
      p[1] = std::byte{other ? std::uint8_t(128) : g};
      p[2] = std::byte{other ? std::uint8_t(128) : b};
      p[3] = std::byte{255};
    }
  return frame_color_stats(pixels, kW, kH, kPitch);
}

void test_stats() {
  const auto grey = solid(100, 100, 100);
  assert(grey.valid);
  assert(grey.mean_r == 100.0 && grey.mean_g == 100.0 && grey.mean_b == 100.0);
  assert(grey.luminance > 99.9 && grey.luminance < 100.1);
  assert(grey.dark_fraction == 0.0 && grey.bright_fraction == 0.0 && grey.green_fraction == 0.0);
  // Padding bytes (0xEE) past each row are not pixels.
  assert(solid(0, 0, 0).dark_fraction == 1.0);
  assert(solid(10, 200, 10).green_fraction == 1.0);
  // Too little data: not valid.
  std::vector<std::byte> short_buffer(10);
  assert(!frame_color_stats(short_buffer, kW, kH, kPitch).valid);
}

void test_classification() {
  const auto scene = solid(90, 110, 120);
  assert(classify_frame_change(scene, solid(95, 108, 118)).empty());
  assert(classify_frame_change(scene, solid(0, 0, 0)) == "black");
  assert(classify_frame_change(scene, solid(255, 255, 255)) == "white");
  assert(classify_frame_change(scene, solid(20, 220, 30)) == "green");
  assert(classify_frame_change(scene, solid(200, 210, 220)) == "brightness jump");
  // A fade that holds black is not flagged again; nor is a first frame.
  assert(classify_frame_change(solid(0, 0, 0), solid(0, 0, 0)).empty());
  assert(classify_frame_change(FrameColorStats{}, solid(0, 0, 0)).empty());
  // A few non-black pixels still count as a black frame.
  assert(classify_frame_change(scene, solid(0, 0, 0, 2)) == "black");
}

}  // namespace

void test_capture_gate() {
  using xenon::core::AnomalyCaptureGate;
  // Off: never arms.
  {
    AnomalyCaptureGate gate(AnomalyCaptureGate::Options{false, 3, 1000});
    assert(!gate.enabled() && !gate.should_arm(0) && gate.armed_count() == 0u);
  }
  // On: the first anomaly arms; others inside the cooldown do not (exactly
  // once per cooldown window); the next after it does; the limit is final.
  AnomalyCaptureGate gate(AnomalyCaptureGate::Options{true, 3, 1000});
  assert(gate.should_arm(5000));
  assert(!gate.should_arm(5001));
  assert(!gate.should_arm(5999));  // 999 ms later: still cooling down
  assert(gate.armed_count() == 1u);
  assert(gate.should_arm(6000));   // exactly the cooldown
  assert(!gate.should_arm(6500));
  assert(gate.should_arm(9000));
  assert(gate.armed_count() == 3u);
  assert(!gate.should_arm(100000));  // max count respected
  assert(!gate.should_arm(200000));
  assert(gate.armed_count() == 3u);
  // A cooldown of zero still honours the count.
  AnomalyCaptureGate burst(AnomalyCaptureGate::Options{true, 2, 0});
  assert(burst.should_arm(1) && burst.should_arm(1) && !burst.should_arm(1));
  // A clock that starts at 0 is not mistaken for "never armed".
  AnomalyCaptureGate early(AnomalyCaptureGate::Options{true, 5, 100});
  assert(early.should_arm(0));
  assert(!early.should_arm(50));
}

int main() {
  test_stats();
  test_classification();
  test_capture_gate();
  return 0;
}
