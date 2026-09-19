#include "xenon/input/keyboard_mouse_driver.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace xenon::input {
namespace {

std::int16_t axis_from_float(float value) {
  value = std::clamp(value, -1.0f, 1.0f);
  if (value >= 0.0f) {
    return static_cast<std::int16_t>(std::lround(value * 32767.0f));
  }
  return static_cast<std::int16_t>(std::lround(value * 32768.0f));
}

void add_axis(float& value, float delta) {
  value = std::clamp(value + delta, -1.0f, 1.0f);
}

std::uint16_t virtual_key_for_control(VirtualControl control) {
  switch (control) {
    case VirtualControl::DpadUp: return 0x5810;
    case VirtualControl::DpadDown: return 0x5811;
    case VirtualControl::DpadLeft: return 0x5812;
    case VirtualControl::DpadRight: return 0x5813;
    case VirtualControl::Start: return 0x5814;
    case VirtualControl::Back: return 0x5815;
    case VirtualControl::LeftThumb: return 0x5816;
    case VirtualControl::RightThumb: return 0x5817;
    case VirtualControl::LeftShoulder: return 0x5818;
    case VirtualControl::RightShoulder: return 0x5819;
    case VirtualControl::ButtonA: return 0x5800;
    case VirtualControl::ButtonB: return 0x5801;
    case VirtualControl::ButtonX: return 0x5802;
    case VirtualControl::ButtonY: return 0x5803;
    case VirtualControl::LeftTrigger: return 0x5806;
    case VirtualControl::RightTrigger: return 0x5807;
    case VirtualControl::Guide:
    case VirtualControl::LeftStickLeft:
    case VirtualControl::LeftStickRight:
    case VirtualControl::LeftStickUp:
    case VirtualControl::LeftStickDown:
    case VirtualControl::RightStickLeft:
    case VirtualControl::RightStickRight:
    case VirtualControl::RightStickUp:
    case VirtualControl::RightStickDown:
      return 0;
  }
  return 0;
}

}  // namespace

KeyboardMouseOptions default_keyboard_mouse_options() {
  KeyboardMouseOptions options{};
  options.key_bindings = {
      {static_cast<KeyCode>(Key::W), VirtualControl::LeftStickUp, 1.0f},
      {static_cast<KeyCode>(Key::S), VirtualControl::LeftStickDown, 1.0f},
      {static_cast<KeyCode>(Key::A), VirtualControl::LeftStickLeft, 1.0f},
      {static_cast<KeyCode>(Key::D), VirtualControl::LeftStickRight, 1.0f},
      {static_cast<KeyCode>(Key::Up), VirtualControl::RightStickUp, 1.0f},
      {static_cast<KeyCode>(Key::Down), VirtualControl::RightStickDown, 1.0f},
      {static_cast<KeyCode>(Key::Left), VirtualControl::RightStickLeft, 1.0f},
      {static_cast<KeyCode>(Key::Right), VirtualControl::RightStickRight, 1.0f},
      {static_cast<KeyCode>(Key::Space), VirtualControl::ButtonA, 1.0f},
      {static_cast<KeyCode>(Key::LeftControl), VirtualControl::ButtonB, 1.0f},
      {static_cast<KeyCode>(Key::Q), VirtualControl::ButtonX, 1.0f},
      {static_cast<KeyCode>(Key::E), VirtualControl::ButtonY, 1.0f},
      {static_cast<KeyCode>(Key::Enter), VirtualControl::Start, 1.0f},
      {static_cast<KeyCode>(Key::Backspace), VirtualControl::Back, 1.0f},
      {static_cast<KeyCode>(Key::LeftShift), VirtualControl::LeftShoulder, 1.0f},
      {static_cast<KeyCode>(Key::R), VirtualControl::RightShoulder, 1.0f},
      {static_cast<KeyCode>(Key::Z), VirtualControl::LeftTrigger, 1.0f},
      {static_cast<KeyCode>(Key::C), VirtualControl::RightTrigger, 1.0f},
  };
  return options;
}

KeyboardMouseInputDriver::KeyboardMouseInputDriver(KeyboardMouseOptions options)
    : options_(std::move(options)) {
  if (options_.key_bindings.empty() && options_.mouse_bindings.empty()) {
    auto defaults = default_keyboard_mouse_options();
    defaults.persistent_key = options_.persistent_key;
    defaults.name = options_.name;
    defaults.mouse_to_right_stick = options_.mouse_to_right_stick;
    defaults.mouse_sensitivity_x = options_.mouse_sensitivity_x;
    defaults.mouse_sensitivity_y = options_.mouse_sensitivity_y;
    defaults.invert_mouse_x = options_.invert_mouse_x;
    defaults.invert_mouse_y = options_.invert_mouse_y;
    defaults.keyboard_axis_scale = options_.keyboard_axis_scale;
    options_ = std::move(defaults);
  }
}

Result KeyboardMouseInputDriver::setup() {
  std::scoped_lock lock(mutex_);
  setup_ = true;
  return Result::Success;
}

void KeyboardMouseInputDriver::shutdown() noexcept {
  std::scoped_lock lock(mutex_);
  setup_ = false;
  keys_.clear();
  mouse_buttons_.clear();
  mouse_dx_ = mouse_dy_ = 0.0f;
  keystrokes_.clear();
}

void KeyboardMouseInputDriver::enumerate_devices(
    std::vector<DriverDeviceInfo>& out_devices) {
  std::scoped_lock lock(mutex_);
  if (!setup_) return;
  DriverDeviceInfo info{};
  info.native_id = kDeviceId;
  info.persistent_key = options_.persistent_key;
  info.name = options_.name;
  info.type = DeviceType::Gamepad;
  info.subtype = DeviceSubtype::Gamepad;
  info.connection = ConnectionType::Virtual;
  info.supports_vibration = false;
  info.supports_keystrokes = true;
  out_devices.push_back(std::move(info));
}

std::uint16_t KeyboardMouseInputDriver::button_mask(
    VirtualControl control) noexcept {
  switch (control) {
    case VirtualControl::ButtonA: return GamepadButton::A;
    case VirtualControl::ButtonB: return GamepadButton::B;
    case VirtualControl::ButtonX: return GamepadButton::X;
    case VirtualControl::ButtonY: return GamepadButton::Y;
    case VirtualControl::DpadUp: return GamepadButton::DpadUp;
    case VirtualControl::DpadDown: return GamepadButton::DpadDown;
    case VirtualControl::DpadLeft: return GamepadButton::DpadLeft;
    case VirtualControl::DpadRight: return GamepadButton::DpadRight;
    case VirtualControl::Start: return GamepadButton::Start;
    case VirtualControl::Back: return GamepadButton::Back;
    case VirtualControl::LeftThumb: return GamepadButton::LeftThumb;
    case VirtualControl::RightThumb: return GamepadButton::RightThumb;
    case VirtualControl::LeftShoulder: return GamepadButton::LeftShoulder;
    case VirtualControl::RightShoulder: return GamepadButton::RightShoulder;
    case VirtualControl::Guide: return GamepadButton::Guide;
    default: return 0;
  }
}

void KeyboardMouseInputDriver::queue_digital_keystrokes_locked(
    VirtualControl control, bool pressed) {
  const auto vk = virtual_key_for_control(control);
  if (vk == 0) return;
  Keystroke key{};
  key.virtual_key = vk;
  key.flags = pressed ? KeystrokeKeyDown : KeystrokeKeyUp;
  keystrokes_.push_back(key);
}

GamepadState KeyboardMouseInputDriver::build_state_locked(bool consume_mouse) {
  GamepadState result{};
  float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
  float lt = 0.0f, rt = 0.0f;

  auto apply = [&](VirtualControl control, float scale) {
    scale = std::clamp(scale, 0.0f, 1.0f);
    const auto mask = button_mask(control);
    if (mask) {
      result.buttons |= mask;
      return;
    }
    switch (control) {
      case VirtualControl::LeftTrigger: lt = std::max(lt, scale); break;
      case VirtualControl::RightTrigger: rt = std::max(rt, scale); break;
      case VirtualControl::LeftStickLeft: add_axis(lx, -scale * options_.keyboard_axis_scale); break;
      case VirtualControl::LeftStickRight: add_axis(lx, scale * options_.keyboard_axis_scale); break;
      case VirtualControl::LeftStickUp: add_axis(ly, scale * options_.keyboard_axis_scale); break;
      case VirtualControl::LeftStickDown: add_axis(ly, -scale * options_.keyboard_axis_scale); break;
      case VirtualControl::RightStickLeft: add_axis(rx, -scale * options_.keyboard_axis_scale); break;
      case VirtualControl::RightStickRight: add_axis(rx, scale * options_.keyboard_axis_scale); break;
      case VirtualControl::RightStickUp: add_axis(ry, scale * options_.keyboard_axis_scale); break;
      case VirtualControl::RightStickDown: add_axis(ry, -scale * options_.keyboard_axis_scale); break;
      default: break;
    }
  };

  for (const auto& binding : options_.key_bindings) {
    if (keys_.contains(binding.key)) apply(binding.control, binding.scale);
  }
  for (const auto& binding : options_.mouse_bindings) {
    if (mouse_buttons_.contains(static_cast<std::uint8_t>(binding.button)))
      apply(binding.control, binding.scale);
  }

  if (options_.mouse_to_right_stick) {
    auto mx = mouse_dx_ * options_.mouse_sensitivity_x / 32767.0f;
    auto my = mouse_dy_ * options_.mouse_sensitivity_y / 32767.0f;
    if (options_.invert_mouse_x) mx = -mx;
    if (!options_.invert_mouse_y) my = -my;
    add_axis(rx, mx);
    add_axis(ry, my);
    if (consume_mouse) mouse_dx_ = mouse_dy_ = 0.0f;
  }

  result.left_trigger = static_cast<std::uint8_t>(std::lround(std::clamp(lt, 0.0f, 1.0f) * 255.0f));
  result.right_trigger = static_cast<std::uint8_t>(std::lround(std::clamp(rt, 0.0f, 1.0f) * 255.0f));
  result.thumb_lx = axis_from_float(lx);
  result.thumb_ly = axis_from_float(ly);
  result.thumb_rx = axis_from_float(rx);
  result.thumb_ry = axis_from_float(ry);
  return result;
}

Result KeyboardMouseInputDriver::get_state(NativeDeviceId device,
                                            GamepadState& out_state) {
  out_state = {};
  std::scoped_lock lock(mutex_);
  if (!setup_ || device != kDeviceId) return Result::DeviceNotConnected;
  out_state = build_state_locked(true);
  return Result::Success;
}

Result KeyboardMouseInputDriver::get_capabilities(NativeDeviceId device,
                                                   Capabilities& out_caps) {
  out_caps = {};
  std::scoped_lock lock(mutex_);
  if (!setup_ || device != kDeviceId) return Result::DeviceNotConnected;
  out_caps.type = DeviceType::Gamepad;
  out_caps.subtype = DeviceSubtype::Gamepad;
  out_caps.flags = CapabilityKeystrokeSupported;
  out_caps.gamepad.buttons = 0xF7FF;
  out_caps.gamepad.left_trigger = 0xFF;
  out_caps.gamepad.right_trigger = 0xFF;
  out_caps.gamepad.thumb_lx = -1;
  out_caps.gamepad.thumb_ly = -1;
  out_caps.gamepad.thumb_rx = -1;
  out_caps.gamepad.thumb_ry = -1;
  return Result::Success;
}

Result KeyboardMouseInputDriver::set_vibration(NativeDeviceId device,
                                                const Vibration& vibration) {
  static_cast<void>(vibration);
  std::scoped_lock lock(mutex_);
  if (!setup_ || device != kDeviceId) return Result::DeviceNotConnected;
  return Result::Unsupported;
}

Result KeyboardMouseInputDriver::get_keystroke(NativeDeviceId device,
                                                Keystroke& out_keystroke) {
  out_keystroke = {};
  std::scoped_lock lock(mutex_);
  if (!setup_ || device != kDeviceId) return Result::DeviceNotConnected;
  if (keystrokes_.empty()) return Result::Empty;
  out_keystroke = keystrokes_.front();
  keystrokes_.pop_front();
  return Result::Success;
}

void KeyboardMouseInputDriver::set_key(KeyCode key, bool pressed) {
  std::scoped_lock lock(mutex_);
  const bool changed = pressed ? keys_.insert(key).second : keys_.erase(key) != 0;
  if (!changed) return;
  for (const auto& binding : options_.key_bindings) {
    if (binding.key == key) queue_digital_keystrokes_locked(binding.control, pressed);
  }
}

void KeyboardMouseInputDriver::set_mouse_button(MouseButton button,
                                                 bool pressed) {
  const auto raw = static_cast<std::uint8_t>(button);
  std::scoped_lock lock(mutex_);
  const bool changed = pressed ? mouse_buttons_.insert(raw).second
                               : mouse_buttons_.erase(raw) != 0;
  if (!changed) return;
  for (const auto& binding : options_.mouse_bindings) {
    if (binding.button == button)
      queue_digital_keystrokes_locked(binding.control, pressed);
  }
}

void KeyboardMouseInputDriver::add_mouse_delta(float x, float y) {
  if (!std::isfinite(x) || !std::isfinite(y)) return;
  std::scoped_lock lock(mutex_);
  mouse_dx_ += x;
  mouse_dy_ += y;
}

void KeyboardMouseInputDriver::clear_input() {
  std::scoped_lock lock(mutex_);
  keys_.clear();
  mouse_buttons_.clear();
  mouse_dx_ = mouse_dy_ = 0.0f;
  keystrokes_.clear();
}

void KeyboardMouseInputDriver::set_options(KeyboardMouseOptions options) {
  std::scoped_lock lock(mutex_);
  options_ = std::move(options);
  keys_.clear();
  mouse_buttons_.clear();
  mouse_dx_ = mouse_dy_ = 0.0f;
  keystrokes_.clear();
}

KeyboardMouseOptions KeyboardMouseInputDriver::options() const {
  std::scoped_lock lock(mutex_);
  return options_;
}

}  // namespace xenon::input
