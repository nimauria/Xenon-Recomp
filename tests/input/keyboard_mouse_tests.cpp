#include <cassert>
#include <cmath>
#include <memory>

#include "xenon/input/keyboard_mouse_driver.hpp"
#include "xenon/input/system.hpp"

namespace input = xenon::input;

namespace {

void test_default_mapping_and_mouse_stick() {
  input::KeyboardMouseInputDriver driver;
  assert(driver.setup() == input::Result::Success);
  std::vector<input::DriverDeviceInfo> devices;
  driver.enumerate_devices(devices);
  assert(devices.size() == 1);
  assert(devices[0].connection == input::ConnectionType::Virtual);
  assert(devices[0].supports_keystrokes);
  assert(!devices[0].supports_vibration);

  driver.set_key(static_cast<input::KeyCode>(input::Key::W), true);
  driver.set_key(static_cast<input::KeyCode>(input::Key::D), true);
  driver.set_key(static_cast<input::KeyCode>(input::Key::Space), true);
  driver.set_key(static_cast<input::KeyCode>(input::Key::Z), true);
  driver.add_mouse_delta(12.0f, -6.0f);

  input::GamepadState state{};
  assert(driver.get_state(1, state) == input::Result::Success);
  assert((state.buttons & input::A) != 0);
  assert(state.left_trigger == 255);
  assert(state.thumb_lx > 30000);
  assert(state.thumb_ly > 30000);
  assert(state.thumb_rx > 0);
  assert(state.thumb_ry > 0);

  input::GamepadState next{};
  assert(driver.get_state(1, next) == input::Result::Success);
  assert(next.thumb_rx == 0 && next.thumb_ry == 0);
  assert(next.thumb_lx > 30000 && next.thumb_ly > 30000);
}

void test_simultaneous_custom_bindings_and_mouse_buttons() {
  input::KeyboardMouseOptions options{};
  options.key_bindings = {
      {42, input::VirtualControl::ButtonA, 1.0f},
      {42, input::VirtualControl::ButtonB, 1.0f},
      {7, input::VirtualControl::LeftStickLeft, 0.5f},
  };
  options.mouse_bindings = {
      {input::MouseButton::Left, input::VirtualControl::RightTrigger, 0.6f},
      {input::MouseButton::Right, input::VirtualControl::RightShoulder, 1.0f},
  };
  options.mouse_to_right_stick = false;
  input::KeyboardMouseInputDriver driver(options);
  assert(driver.setup() == input::Result::Success);

  driver.set_key(42, true);
  driver.set_key(7, true);
  driver.set_mouse_button(input::MouseButton::Left, true);
  driver.set_mouse_button(input::MouseButton::Right, true);
  input::GamepadState state{};
  assert(driver.get_state(1, state) == input::Result::Success);
  assert((state.buttons & input::A) != 0);
  assert((state.buttons & input::B) != 0);
  assert((state.buttons & input::RightShoulder) != 0);
  assert(state.right_trigger >= 152 && state.right_trigger <= 154);
  assert(state.thumb_lx < -16000 && state.thumb_lx > -17000);
}

void test_keystroke_queue() {
  input::KeyboardMouseInputDriver driver;
  assert(driver.setup() == input::Result::Success);
  const auto space = static_cast<input::KeyCode>(input::Key::Space);
  driver.set_key(space, true);
  driver.set_key(space, true);  // duplicate host event must not duplicate keydown.
  input::Keystroke key{};
  assert(driver.get_keystroke(1, key) == input::Result::Success);
  assert(key.virtual_key == 0x5800);
  assert(key.flags == input::KeystrokeKeyDown);
  assert(driver.get_keystroke(1, key) == input::Result::Empty);
  driver.set_key(space, false);
  assert(driver.get_keystroke(1, key) == input::Result::Success);
  assert(key.virtual_key == 0x5800);
  assert(key.flags == input::KeystrokeKeyUp);
}

void test_input_system_virtual_device_integration() {
  input::InputSystem system;
  auto driver = std::make_unique<input::KeyboardMouseInputDriver>();
  auto* keyboard = driver.get();
  assert(system.add_driver(std::move(driver)));
  assert(system.setup() == input::Result::Success);
  assert(system.device_for_user(0).has_value());

  keyboard->set_key(static_cast<input::KeyCode>(input::Key::Space), true);
  input::State first{};
  assert(system.get_state(0, first) == input::Result::Success);
  assert((first.gamepad.buttons & input::A) != 0);
  input::State same{};
  assert(system.get_state(0, same) == input::Result::Success);
  assert(same.packet_number == first.packet_number);
}

}  // namespace

int main() {
  test_default_mapping_and_mouse_stick();
  test_simultaneous_custom_bindings_and_mouse_buttons();
  test_keystroke_queue();
  test_input_system_virtual_device_integration();
  return 0;
}
