#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "xenon/input/driver.hpp"

namespace xenon::input {

using KeyCode = std::uint16_t;

enum class Key : KeyCode {
  A = 0x04,
  B = 0x05,
  C = 0x06,
  D = 0x07,
  E = 0x08,
  Q = 0x14,
  R = 0x15,
  S = 0x16,
  W = 0x1A,
  Z = 0x1D,
  Enter = 0x28,
  Escape = 0x29,
  Backspace = 0x2A,
  Space = 0x2C,
  Right = 0x4F,
  Left = 0x50,
  Down = 0x51,
  Up = 0x52,
  LeftControl = 0xE0,
  LeftShift = 0xE1,
  LeftAlt = 0xE2,
  RightControl = 0xE4,
  RightShift = 0xE5,
};

enum class MouseButton : std::uint8_t {
  Left = 1,
  Right = 2,
  Middle = 3,
  X1 = 4,
  X2 = 5,
};

enum class VirtualControl : std::uint8_t {
  ButtonA,
  ButtonB,
  ButtonX,
  ButtonY,
  DpadUp,
  DpadDown,
  DpadLeft,
  DpadRight,
  Start,
  Back,
  LeftThumb,
  RightThumb,
  LeftShoulder,
  RightShoulder,
  Guide,
  LeftTrigger,
  RightTrigger,
  LeftStickLeft,
  LeftStickRight,
  LeftStickUp,
  LeftStickDown,
  RightStickLeft,
  RightStickRight,
  RightStickUp,
  RightStickDown,
};

struct KeyBinding {
  KeyCode key{};
  VirtualControl control{};
  float scale{1.0f};
};

struct MouseButtonBinding {
  MouseButton button{MouseButton::Left};
  VirtualControl control{};
  float scale{1.0f};
};

struct KeyboardMouseOptions {
  std::string persistent_key{"keyboard-mouse:default"};
  std::string name{"Keyboard + Mouse"};
  std::vector<KeyBinding> key_bindings{};
  std::vector<MouseButtonBinding> mouse_bindings{};
  bool mouse_to_right_stick{true};
  float mouse_sensitivity_x{900.0f};
  float mouse_sensitivity_y{900.0f};
  bool invert_mouse_x{};
  bool invert_mouse_y{};
  float keyboard_axis_scale{1.0f};
};

[[nodiscard]] KeyboardMouseOptions default_keyboard_mouse_options();

// Host-neutral virtual Xbox controller. A frontend/platform backend feeds it
// key/button/mouse-delta events; the driver itself owns no window-system API.
class KeyboardMouseInputDriver final : public InputDriver {
 public:
  explicit KeyboardMouseInputDriver(KeyboardMouseOptions options = {});

  [[nodiscard]] std::string_view name() const noexcept override {
    return "keyboard-mouse";
  }
  [[nodiscard]] Result setup() override;
  void shutdown() noexcept override;
  void enumerate_devices(std::vector<DriverDeviceInfo>& out_devices) override;
  [[nodiscard]] Result get_state(NativeDeviceId device,
                                 GamepadState& out_state) override;
  [[nodiscard]] Result get_capabilities(NativeDeviceId device,
                                        Capabilities& out_caps) override;
  [[nodiscard]] Result set_vibration(NativeDeviceId device,
                                     const Vibration& vibration) override;
  [[nodiscard]] Result get_keystroke(NativeDeviceId device,
                                     Keystroke& out_keystroke) override;

  void set_key(KeyCode key, bool pressed);
  void set_mouse_button(MouseButton button, bool pressed);
  void add_mouse_delta(float x, float y);
  void clear_input();
  void set_options(KeyboardMouseOptions options);
  [[nodiscard]] KeyboardMouseOptions options() const;

 private:
  [[nodiscard]] GamepadState build_state_locked(bool consume_mouse);
  void queue_digital_keystrokes_locked(VirtualControl control, bool pressed);
  [[nodiscard]] static std::uint16_t button_mask(VirtualControl control) noexcept;

  static constexpr NativeDeviceId kDeviceId = 1;
  mutable std::mutex mutex_{};
  KeyboardMouseOptions options_{};
  std::unordered_set<KeyCode> keys_{};
  std::unordered_set<std::uint8_t> mouse_buttons_{};
  float mouse_dx_{};
  float mouse_dy_{};
  std::deque<Keystroke> keystrokes_{};
  bool setup_{};
};

}  // namespace xenon::input
