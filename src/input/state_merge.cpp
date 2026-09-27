#include "xenon/input/state_merge.hpp"

#include <algorithm>
#include <cstdlib>

namespace xenon::input {
namespace {
std::int16_t larger_magnitude(std::int16_t a, std::int16_t b) noexcept {
  return std::abs(static_cast<int>(a)) >= std::abs(static_cast<int>(b)) ? a : b;
}
}

void merge_gamepad_state(GamepadState& dst, const GamepadState& src) noexcept {
  dst.buttons = static_cast<std::uint16_t>(dst.buttons | src.buttons);
  dst.left_trigger = std::max(dst.left_trigger, src.left_trigger);
  dst.right_trigger = std::max(dst.right_trigger, src.right_trigger);
  dst.thumb_lx = larger_magnitude(dst.thumb_lx, src.thumb_lx);
  dst.thumb_ly = larger_magnitude(dst.thumb_ly, src.thumb_ly);
  dst.thumb_rx = larger_magnitude(dst.thumb_rx, src.thumb_rx);
  dst.thumb_ry = larger_magnitude(dst.thumb_ry, src.thumb_ry);
}

bool gamepad_is_neutral(const GamepadState& state, std::int16_t deadzone,
                        std::uint8_t trigger_threshold) noexcept {
  if (state.buttons != 0 || state.left_trigger >= trigger_threshold ||
      state.right_trigger >= trigger_threshold) return false;
  const auto past = [deadzone](std::int16_t v) {
    return std::abs(static_cast<int>(v)) >= std::abs(static_cast<int>(deadzone));
  };
  return !past(state.thumb_lx) && !past(state.thumb_ly) &&
         !past(state.thumb_rx) && !past(state.thumb_ry);
}

}  // namespace xenon::input
