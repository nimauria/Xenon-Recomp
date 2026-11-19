// Module input profiles: per-title keyboard/mouse contexts (menus, flight...)
// selected at run time, with mouse steering. Without them a title played on
// a keyboard either had no stick at all (AC6's aircraft took no input) or one
// layout for every part of the game.

#include <cassert>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

#include "xenon/input/keyboard_mouse_driver.hpp"
#include "xenon/input/module_input_profile.hpp"

namespace input = xenon::input;

namespace {

input::KeyCode key(const char* name) {
  const auto code = input::key_from_name(name);
  assert(code);
  return *code;
}

void test_names() {
  assert(key("A") == 0x04 && key("z") == 0x1D);
  assert(key("1") == 0x1E && key("0") == 0x27);
  assert(key("F1") == 0x3A && key("f12") == 0x45);
  assert(key("Numpad8") == 0x60 && key("numpad0") == 0x62);
  assert(key("LeftAlt") == 0xE2 && key("Tab") == 0x2B && key("Escape") == 0x29);
  assert(!input::key_from_name("F13") && !input::key_from_name("Nope"));
  assert(input::virtual_control_from_name("lefttrigger") == input::VirtualControl::LeftTrigger);
  assert(!input::virtual_control_from_name("Trigger"));
  assert(input::mouse_button_from_name("X2") == input::MouseButton::X2);
}

constexpr const char* kProfile = R"({
  "schema": "xenon.input.v1",
  "keyboard": {
    "initialContext": "menu",
    "contexts": [
      { "name": "menu", "keys": [["W", "DpadUp"], ["Space", "ButtonA"]] },
      { "name": "flight",
        "keys": [["W", "RightTrigger"], ["A", "LeftStickLeft"], ["2", "LeftTrigger"],
                 ["2", "RightTrigger"], ["Q", "LeftShoulder", 0.5]],
        "mouseButtons": [["Left", "ButtonA"]],
        "mouse": { "stick": "left", "sensitivityX": 2, "sensitivityY": 2, "invertY": true,
                   "smoothing": 0, "antiDeadzone": 0.15, "alternateStickKey": "LeftAlt" } }
    ]
  }
})";

void test_parse() {
  std::string error;
  const auto profile = input::parse_module_input_profile(kProfile, &error);
  assert(profile && error.empty());
  assert(profile->initial_context == "menu");
  assert(profile->keyboard_contexts.size() == 2);
  const auto& flight = profile->keyboard_contexts[1];
  assert(flight.key_bindings.size() == 5);
  assert(flight.key_bindings[4].scale == 0.5f);
  assert(flight.mouse_bindings.size() == 1);
  assert(flight.mouse.stick == input::MouseStick::Left && flight.mouse.invert_y);
  assert(flight.mouse.alternate_stick_key == key("LeftAlt"));

  const auto rejects = [](const char* json, const char* expected) {
    std::string why;
    assert(!input::parse_module_input_profile(json, &why));
    assert(why.find(expected) != std::string::npos);
  };
  rejects(R"({"schema":"xenon.input.v0"})", "schema");
  rejects(R"({"schema":"xenon.input.v1","keyboard":{"contexts":[{"name":"a","keys":[["Nope","ButtonA"]]}]}})",
          "unknown input");
  rejects(R"({"schema":"xenon.input.v1","keyboard":{"contexts":[{"name":"a","keys":[["A","Fire"]]}]}})",
          "unknown control");
  rejects(R"({"schema":"xenon.input.v1","keyboard":{"contexts":[{"name":"a"},{"name":"a"}]}})",
          "defined twice");
  rejects(R"({"schema":"xenon.input.v1","keyboard":{"initialContext":"b","contexts":[{"name":"a"}]}})",
          "initialContext");
  rejects(R"({"schema":"xenon.input.v1","keyboard":{"contexts":[{"name":"a","mouse":{"stick":"up"}}]}})",
          "stick");
}

input::KeyboardMouseInputDriver flight_driver() {
  auto options = input::default_keyboard_mouse_options();
  options.mouse_to_right_stick = false;
  std::string error;
  input::apply_module_input_profile(*input::parse_module_input_profile(kProfile, &error), options);
  return input::KeyboardMouseInputDriver(std::move(options));
}

void test_contexts_switch_bindings() {
  auto driver = flight_driver();
  assert(driver.setup() == input::Result::Success);
  assert(driver.context().empty());
  assert(!driver.set_context("swimming"));
  assert(driver.context().empty());

  // Default bindings: W is the D-pad.
  driver.set_key(key("W"), true);
  input::GamepadState state{};
  assert(driver.get_state(1, state) == input::Result::Success);
  assert((state.buttons & input::DpadUp) != 0 && state.right_trigger == 0);

  // Held through the switch: the D-pad keystroke is released and the trigger
  // keystroke pressed, so neither stays latched.
  input::Keystroke stroke{};
  while (driver.get_keystroke(1, stroke) == input::Result::Success) {}
  assert(driver.set_context("flight"));
  assert(driver.context() == "flight");
  assert(driver.get_keystroke(1, stroke) == input::Result::Success);
  assert(stroke.virtual_key == 0x5810 && stroke.flags == input::KeystrokeKeyUp);
  assert(driver.get_keystroke(1, stroke) == input::Result::Success);
  assert(stroke.virtual_key == 0x5807 && stroke.flags == input::KeystrokeKeyDown);
  assert(driver.get_state(1, state) == input::Result::Success);
  assert((state.buttons & input::DpadUp) == 0 && state.right_trigger == 255);

  // One key driving two controls (AC7's high-G: both triggers).
  driver.set_key(key("W"), false);
  driver.set_key(key("2"), true);
  driver.set_key(key("Q"), true);
  assert(driver.get_state(1, state) == input::Result::Success);
  assert(state.left_trigger == 255 && state.right_trigger == 255);
  assert((state.buttons & input::LeftShoulder) != 0);

  // Back to the default bindings.
  driver.clear_input();
  assert(driver.set_context(""));
  driver.set_key(key("W"), true);
  assert(driver.get_state(1, state) == input::Result::Success);
  assert((state.buttons & input::DpadUp) != 0);
}

void test_mouse_steering() {
  auto driver = flight_driver();
  assert(driver.setup() == input::Result::Success);
  assert(!driver.wants_mouse_capture());
  assert(driver.set_context("flight"));
  assert(driver.wants_mouse_capture());

  input::GamepadState state{};
  assert(driver.get_state(1, state) == input::Result::Success);  // starts the clock
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  // Right and down: right roll, and with invert_y the stick goes forward.
  driver.add_mouse_delta(40.0f, 40.0f);
  assert(driver.get_state(1, state) == input::Result::Success);
  assert(state.thumb_lx > 0 && state.thumb_ly > 0);
  assert(state.thumb_rx == 0 && state.thumb_ry == 0);
  // Any motion starts past the anti-deadzone.
  assert(state.thumb_lx >= static_cast<std::int16_t>(0.15f * 32767.0f));

  // The mouse stopped: the stick recentres (no smoothing in this profile).
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(driver.get_state(1, state) == input::Result::Success);
  assert(state.thumb_lx == 0 && state.thumb_ly == 0);

  // Holding the alternate key moves the mouse onto the other stick.
  driver.set_key(key("LeftAlt"), true);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  driver.add_mouse_delta(-40.0f, 0.0f);
  assert(driver.get_state(1, state) == input::Result::Success);
  assert(state.thumb_rx < 0 && state.thumb_lx == 0);

  assert(driver.set_context("menu"));
  assert(!driver.wants_mouse_capture());
}

}  // namespace

int main() {
  test_names();
  test_parse();
  test_contexts_switch_bindings();
  test_mouse_steering();
  return 0;
}
