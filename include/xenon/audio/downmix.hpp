#pragma once

#include <utility>

namespace xenon::audio {

// Gain of the center and each surround channel when 5.1 folds into stereo:
// -3 dB, the equal-power share of one source split across two speakers.
inline constexpr float kDownmixFoldGain = 0.70710678f;

// Folds one frame of Xbox 5.1 (FL, FR, C, LFE, BL, BR) into stereo with the
// Lo/Ro matrix consoles and receivers use for a stereo output:
//   L = FL + 0.707 C + 0.707 BL,  R = FR + 0.707 C + 0.707 BR,
// LFE dropped. Fronts pass at unity, so a title's mostly-front mix keeps its
// level; the host output's final clamp handles the rare full-scale overlap.
// (The 0.4-scaled fold this replaces is SDL's generic conversion; it cost
// such mixes 8 dB.)
[[nodiscard]] constexpr std::pair<float, float> downmix_5_1_to_stereo(
    float front_left, float front_right, float center, float back_left,
    float back_right) noexcept {
  const float folded_center = center * kDownmixFoldGain;
  return {front_left + folded_center + back_left * kDownmixFoldGain,
          front_right + folded_center + back_right * kDownmixFoldGain};
}

}  // namespace xenon::audio
