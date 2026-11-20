#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/input/driver.hpp"

namespace xenon::input {

// A virtual controller that plays a timed input script, so a run can reach a
// known game state (a menu, a mission) without anyone at the controls:
// automated regression runs, performance captures, timing measurements.
//
// Script text, one event per line ('#' starts a comment):
//
//   <start> <action> [for <duration>]
//   <start> repeat <count> every <interval> <action> [for <duration>]
//
// <start> is a time from the clock's zero ("12s", "1500ms", "2.5s") or, with a
// leading '+', from the previous line's start ("+2s"). The clock's zero is the
// title's first poll of this controller (the first XInputGetState that reaches
// it), so loading before the title reads input does not shift the script.
// Actions:
//   press <buttons>          buttons joined by '+': A B X Y START BACK LB RB
//                            LS RS UP DOWN LEFT RIGHT GUIDE
//   stick <left|right> <x> <y>   axis values in [-1, 1]
//   trigger <left|right> <v>     value in [0, 1]
//   log <text>               a marker reported when reached (no input)
// The default duration is 100 ms. While events overlap, buttons combine and
// each analog axis takes the most recently started event's value.
struct ScriptedInputEvent {
  enum class Kind : std::uint8_t { Buttons, LeftStick, RightStick, LeftTrigger, RightTrigger, Log };
  Kind kind{Kind::Buttons};
  std::chrono::milliseconds start{};
  std::chrono::milliseconds duration{};
  std::uint16_t buttons{};
  std::int16_t x{};
  std::int16_t y{};
  std::uint8_t trigger{};
  std::string text{};  // the script line (or the log text), for reporting
  std::uint32_t line{};
};

struct ScriptedInput {
  std::vector<ScriptedInputEvent> events{};  // ordered by start
  [[nodiscard]] std::chrono::milliseconds end() const noexcept;
};

// Parses script text. On failure returns nullopt and sets `error` to the line
// number and the reason.
[[nodiscard]] std::optional<ScriptedInput> parse_input_script(std::string_view text,
                                                              std::string& error);

// The controller state the script holds at `at` from its zero.
[[nodiscard]] GamepadState scripted_state_at(const ScriptedInput& script,
                                             std::chrono::milliseconds at) noexcept;

class ScriptedInputDriver final : public InputDriver {
 public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;
  // Called once per event as the script reaches it, with the event and the
  // time it was reached.
  using Reporter = std::function<void(const ScriptedInputEvent&, std::chrono::milliseconds)>;

  static constexpr std::string_view kPersistentKey = "script";

  explicit ScriptedInputDriver(ScriptedInput script, Clock clock = {}, Reporter reporter = {});

  [[nodiscard]] std::string_view name() const noexcept override { return "script"; }
  [[nodiscard]] Result setup() override { return Result::Success; }
  void shutdown() noexcept override {}
  void enumerate_devices(std::vector<DriverDeviceInfo>& out_devices) override;
  [[nodiscard]] Result get_state(NativeDeviceId device, GamepadState& out_state) override;
  [[nodiscard]] Result get_capabilities(NativeDeviceId device, Capabilities& out_caps) override;
  [[nodiscard]] Result set_vibration(NativeDeviceId device, const Vibration& vibration) override;
  [[nodiscard]] Result get_keystroke(NativeDeviceId device, Keystroke& out_keystroke) override;

  // Time since the clock's zero; nullopt before the first poll.
  [[nodiscard]] std::optional<std::chrono::milliseconds> elapsed() const;
  // Events reached so far.
  [[nodiscard]] std::size_t events_reached() const;

 private:
  static constexpr NativeDeviceId kDevice = 1;

  ScriptedInput script_;
  Clock clock_;
  Reporter reporter_;
  mutable std::mutex mutex_{};
  std::optional<std::chrono::steady_clock::time_point> zero_{};
  std::size_t reached_{};
};

}  // namespace xenon::input
