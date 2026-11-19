#include "xenon/input/keyboard_mouse_driver.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

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

bool same_name(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

// HID keyboard usages (what SDL scancodes are) for the named keys.
constexpr std::array<std::pair<std::string_view, KeyCode>, 36> kNamedKeys{{
    {"Enter", 0x28}, {"Return", 0x28}, {"Escape", 0x29}, {"Esc", 0x29},
    {"Backspace", 0x2A}, {"Tab", 0x2B}, {"Space", 0x2C}, {"Minus", 0x2D},
    {"Equals", 0x2E}, {"LeftBracket", 0x2F}, {"RightBracket", 0x30},
    {"Backslash", 0x31}, {"Semicolon", 0x33}, {"Apostrophe", 0x34}, {"Grave", 0x35},
    {"Comma", 0x36}, {"Period", 0x37}, {"Slash", 0x38}, {"CapsLock", 0x39},
    {"Insert", 0x49}, {"Home", 0x4A}, {"PageUp", 0x4B}, {"Delete", 0x4C},
    {"End", 0x4D}, {"PageDown", 0x4E}, {"Right", 0x4F}, {"Left", 0x50},
    {"Down", 0x51}, {"Up", 0x52}, {"LeftControl", 0xE0}, {"LeftShift", 0xE1},
    {"LeftAlt", 0xE2}, {"RightControl", 0xE4}, {"RightShift", 0xE5},
    {"RightAlt", 0xE6}, {"NumpadEnter", 0x58},
}};

constexpr std::array<std::pair<std::string_view, VirtualControl>, 25> kNamedControls{{
    {"ButtonA", VirtualControl::ButtonA}, {"ButtonB", VirtualControl::ButtonB},
    {"ButtonX", VirtualControl::ButtonX}, {"ButtonY", VirtualControl::ButtonY},
    {"DpadUp", VirtualControl::DpadUp}, {"DpadDown", VirtualControl::DpadDown},
    {"DpadLeft", VirtualControl::DpadLeft}, {"DpadRight", VirtualControl::DpadRight},
    {"Start", VirtualControl::Start}, {"Back", VirtualControl::Back},
    {"LeftThumb", VirtualControl::LeftThumb}, {"RightThumb", VirtualControl::RightThumb},
    {"LeftShoulder", VirtualControl::LeftShoulder},
    {"RightShoulder", VirtualControl::RightShoulder}, {"Guide", VirtualControl::Guide},
    {"LeftTrigger", VirtualControl::LeftTrigger}, {"RightTrigger", VirtualControl::RightTrigger},
    {"LeftStickLeft", VirtualControl::LeftStickLeft},
    {"LeftStickRight", VirtualControl::LeftStickRight},
    {"LeftStickUp", VirtualControl::LeftStickUp},
    {"LeftStickDown", VirtualControl::LeftStickDown},
    {"RightStickLeft", VirtualControl::RightStickLeft},
    {"RightStickRight", VirtualControl::RightStickRight},
    {"RightStickUp", VirtualControl::RightStickUp},
    {"RightStickDown", VirtualControl::RightStickDown},
}};

}  // namespace

std::optional<KeyCode> key_from_name(std::string_view name) noexcept {
  if (name.size() == 1) {
    const auto c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    if (c >= 'A' && c <= 'Z') return static_cast<KeyCode>(0x04 + (c - 'A'));
    if (c >= '1' && c <= '9') return static_cast<KeyCode>(0x1E + (c - '1'));
    if (c == '0') return KeyCode{0x27};
  }
  if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f')) {
    unsigned n = 0;
    bool digits = true;
    for (std::size_t i = 1; i < name.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
        digits = false;
        break;
      }
      n = n * 10u + static_cast<unsigned>(name[i] - '0');
    }
    if (digits && n >= 1u && n <= 12u) return static_cast<KeyCode>(0x3A + (n - 1u));
  }
  constexpr std::string_view kNumpad = "Numpad";
  if (name.size() == kNumpad.size() + 1 && same_name(name.substr(0, kNumpad.size()), kNumpad)) {
    const char c = name.back();
    if (c >= '1' && c <= '9') return static_cast<KeyCode>(0x59 + (c - '1'));
    if (c == '0') return KeyCode{0x62};
  }
  for (const auto& [key_name, code] : kNamedKeys)
    if (same_name(name, key_name)) return code;
  return std::nullopt;
}

std::optional<VirtualControl> virtual_control_from_name(std::string_view name) noexcept {
  for (const auto& [control_name, control] : kNamedControls)
    if (same_name(name, control_name)) return control;
  return std::nullopt;
}

std::optional<MouseButton> mouse_button_from_name(std::string_view name) noexcept {
  if (same_name(name, "Left")) return MouseButton::Left;
  if (same_name(name, "Right")) return MouseButton::Right;
  if (same_name(name, "Middle")) return MouseButton::Middle;
  if (same_name(name, "X1")) return MouseButton::X1;
  if (same_name(name, "X2")) return MouseButton::X2;
  return std::nullopt;
}

KeyboardMouseOptions default_keyboard_mouse_options() {
  KeyboardMouseOptions options{};
  // The PC menu conventions AC7 established (and the AC6 static recomp's
  // keyboard layer follows): WASD navigates, Space/Enter or the left button
  // confirm, Escape/Backspace or the right button cancel, Enter is Start and
  // Tab is Back. The sticks need keys too or a title's gameplay is
  // unreachable (AC6 flies on the left stick, and with no axis bound the
  // aircraft took no input at all): the arrow keys are the left stick and
  // IJKL the right stick. They are kept off the WASD keys because many
  // titles give the D-pad its own gameplay meaning (AC6: wingman commands).
  options.key_bindings = {
      {static_cast<KeyCode>(Key::W), VirtualControl::DpadUp, 1.0f},
      {static_cast<KeyCode>(Key::S), VirtualControl::DpadDown, 1.0f},
      {static_cast<KeyCode>(Key::A), VirtualControl::DpadLeft, 1.0f},
      {static_cast<KeyCode>(Key::D), VirtualControl::DpadRight, 1.0f},
      {static_cast<KeyCode>(Key::Up), VirtualControl::LeftStickUp, 1.0f},
      {static_cast<KeyCode>(Key::Down), VirtualControl::LeftStickDown, 1.0f},
      {static_cast<KeyCode>(Key::Left), VirtualControl::LeftStickLeft, 1.0f},
      {static_cast<KeyCode>(Key::Right), VirtualControl::LeftStickRight, 1.0f},
      {static_cast<KeyCode>(Key::I), VirtualControl::RightStickUp, 1.0f},
      {static_cast<KeyCode>(Key::K), VirtualControl::RightStickDown, 1.0f},
      {static_cast<KeyCode>(Key::J), VirtualControl::RightStickLeft, 1.0f},
      {static_cast<KeyCode>(Key::L), VirtualControl::RightStickRight, 1.0f},
      {static_cast<KeyCode>(Key::Space), VirtualControl::ButtonA, 1.0f},
      {static_cast<KeyCode>(Key::Enter), VirtualControl::ButtonA, 1.0f},
      {static_cast<KeyCode>(Key::Escape), VirtualControl::ButtonB, 1.0f},
      {static_cast<KeyCode>(Key::Backspace), VirtualControl::ButtonB, 1.0f},
      {static_cast<KeyCode>(Key::Q), VirtualControl::ButtonX, 1.0f},
      {static_cast<KeyCode>(Key::E), VirtualControl::ButtonY, 1.0f},
      {static_cast<KeyCode>(Key::Enter), VirtualControl::Start, 1.0f},
      {static_cast<KeyCode>(Key::Tab), VirtualControl::Back, 1.0f},
      {static_cast<KeyCode>(Key::LeftShift), VirtualControl::LeftShoulder, 1.0f},
      {static_cast<KeyCode>(Key::R), VirtualControl::RightShoulder, 1.0f},
      {static_cast<KeyCode>(Key::Z), VirtualControl::LeftTrigger, 1.0f},
      {static_cast<KeyCode>(Key::C), VirtualControl::RightTrigger, 1.0f},
  };
  options.mouse_bindings = {
      {MouseButton::Left, VirtualControl::ButtonA, 1.0f},
      {MouseButton::Right, VirtualControl::ButtonB, 1.0f},
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
  info.family = ControllerFamily::Virtual;
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

const KeyboardMouseContext* KeyboardMouseInputDriver::active_context_locked() const noexcept {
  if (context_index_ < 0 || static_cast<std::size_t>(context_index_) >= options_.contexts.size())
    return nullptr;
  return &options_.contexts[static_cast<std::size_t>(context_index_)];
}

const std::vector<KeyBinding>& KeyboardMouseInputDriver::active_keys_locked() const noexcept {
  const auto* context = active_context_locked();
  return context ? context->key_bindings : options_.key_bindings;
}

const std::vector<MouseButtonBinding>& KeyboardMouseInputDriver::active_mouse_buttons_locked()
    const noexcept {
  const auto* context = active_context_locked();
  return context ? context->mouse_bindings : options_.mouse_bindings;
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

  for (const auto& binding : active_keys_locked()) {
    if (keys_.contains(binding.key)) apply(binding.control, binding.scale);
  }
  for (const auto& binding : active_mouse_buttons_locked()) {
    if (mouse_buttons_.contains(static_cast<std::uint8_t>(binding.button)))
      apply(binding.control, binding.scale);
  }

  const auto* context = active_context_locked();
  if (context && context->mouse.stick != MouseStick::None) {
    // Steering: the stick follows the mouse's speed over the time since the
    // last poll, through a first-order filter so sensor jitter does not
    // shake it, and returns to centre when the mouse stops.
    const auto& steering = context->mouse;
    const auto now = std::chrono::steady_clock::now();
    const float dt = steer_time_ == std::chrono::steady_clock::time_point{}
                         ? 0.0f
                         : std::chrono::duration<float>(now - steer_time_).count();
    if (consume_mouse) {
      steer_time_ = now;
      if (dt > 0.0f) {
        const float speed_x = mouse_dx_ / dt;
        const float speed_y = mouse_dy_ / dt;
        float target_x = std::clamp(speed_x * steering.sensitivity_x / 1000.0f, -1.0f, 1.0f);
        float target_y = std::clamp(speed_y * steering.sensitivity_y / 1000.0f, -1.0f, 1.0f);
        if (steering.invert_x) target_x = -target_x;
        // Screen Y grows downward and a stick's Y grows forward, so moving
        // the mouse down pulls back unless invert_y is set.
        if (steering.invert_y) target_y = -target_y;
        const float blend = steering.smoothing_seconds > 0.0f
                                ? 1.0f - std::exp(-dt / steering.smoothing_seconds)
                                : 1.0f;
        steer_x_ += (target_x - steer_x_) * blend;
        steer_y_ += (target_y - steer_y_) * blend;
      }
      mouse_dx_ = mouse_dy_ = 0.0f;
    }
    const auto shaped = [&](float v) {
      if (std::fabs(v) < 1e-4f) return 0.0f;
      const float magnitude =
          steering.anti_deadzone + (1.0f - steering.anti_deadzone) * std::min(std::fabs(v), 1.0f);
      return std::copysign(magnitude, v);
    };
    const bool alternate =
        steering.alternate_stick_key != 0 && keys_.contains(steering.alternate_stick_key);
    const bool left = (steering.stick == MouseStick::Left) != alternate;
    add_axis(left ? lx : rx, shaped(steer_x_));
    add_axis(left ? ly : ry, -shaped(steer_y_));
  } else if (!context && options_.mouse_to_right_stick) {
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
  for (const auto& binding : active_keys_locked()) {
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
  for (const auto& binding : active_mouse_buttons_locked()) {
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
  context_index_ = -1;
  steer_x_ = steer_y_ = 0.0f;
  steer_time_ = {};
  keys_.clear();
  mouse_buttons_.clear();
  mouse_dx_ = mouse_dy_ = 0.0f;
  keystrokes_.clear();
}

KeyboardMouseOptions KeyboardMouseInputDriver::options() const {
  std::scoped_lock lock(mutex_);
  return options_;
}

bool KeyboardMouseInputDriver::set_context(std::string_view name) {
  std::scoped_lock lock(mutex_);
  int index = -1;
  if (!name.empty()) {
    for (std::size_t i = 0; i < options_.contexts.size(); ++i) {
      if (options_.contexts[i].name == name) {
        index = static_cast<int>(i);
        break;
      }
    }
    if (index < 0) return false;
  }
  if (index == context_index_) return true;
  // Held keys keep their physical state; their keystroke edges move from
  // the old bindings to the new ones, so a control held through the switch
  // is neither stuck down nor pressed twice.
  for (const auto key : keys_)
    for (const auto& binding : active_keys_locked())
      if (binding.key == key) queue_digital_keystrokes_locked(binding.control, false);
  context_index_ = index;
  for (const auto key : keys_)
    for (const auto& binding : active_keys_locked())
      if (binding.key == key) queue_digital_keystrokes_locked(binding.control, true);
  mouse_dx_ = mouse_dy_ = 0.0f;
  steer_x_ = steer_y_ = 0.0f;
  steer_time_ = {};
  return true;
}

std::string KeyboardMouseInputDriver::context() const {
  std::scoped_lock lock(mutex_);
  const auto* context = active_context_locked();
  return context ? context->name : std::string{};
}

bool KeyboardMouseInputDriver::wants_mouse_capture() const {
  std::scoped_lock lock(mutex_);
  const auto* context = active_context_locked();
  return context && context->mouse.stick != MouseStick::None;
}

}  // namespace xenon::input
