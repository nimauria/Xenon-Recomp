#pragma once

#include "xenon/input/types.hpp"

namespace xenon::input {

// Xbox-style composite input: buttons are ORed, triggers take the strongest
// source and each stick axis keeps the value with the largest magnitude.
void merge_gamepad_state(GamepadState& destination,
                         const GamepadState& source) noexcept;
[[nodiscard]] bool gamepad_is_neutral(const GamepadState& state,
                                      std::int16_t thumb_deadzone = 7849,
                                      std::uint8_t trigger_threshold = 30) noexcept;

}  // namespace xenon::input
