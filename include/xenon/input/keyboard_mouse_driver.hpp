#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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
  I = 0x0C,
  J = 0x0D,
  K = 0x0E,
  L = 0x0F,
  Q = 0x14,
  R = 0x15,
  S = 0x16,
  W = 0x1A,
  Z = 0x1D,
  Enter = 0x28,
  Escape = 0x29,
  Backspace = 0x2A,
  Tab = 0x2B,
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

// Which stick mouse motion deflects while a context is active.
enum class MouseStick : std::uint8_t { None, Left, Right };

// How mouse motion becomes a stick deflection in a context that steers with
// it (the AC7 / Project Wingman model): the deflection follows the mouse's
// current speed and recentres as soon as it stops, so it never needs to be
// "returned" to centre.
struct MouseSteering {
  MouseStick stick{MouseStick::None};
  // Full deflection at 1000 / sensitivity pixels per second.
  float sensitivity_x{1.5f};
  float sensitivity_y{1.5f};
  bool invert_x{};
  // true: moving the mouse down pushes the stick forward (nose down).
  bool invert_y{};
  // Time constant of the speed filter, in seconds; 0 follows raw motion.
  float smoothing_seconds{0.035f};
  // Any non-zero deflection starts past this fraction, so a small motion is
  // not lost in the title's own stick deadzone.
  float anti_deadzone{0.15f};
  // While this key is held the mouse drives the other stick instead (a
  // camera-look key); 0 = none.
  KeyCode alternate_stick_key{};
};

// A named binding set a title switches to for one part of the game (menus,
// flying, driving...). The title module selects it at run time through the
// module services (cpu::ModuleServicesV1::set_input_context); the bindings
// come from the module's input profile (input.json).
struct KeyboardMouseContext {
  std::string name{};
  std::vector<KeyBinding> key_bindings{};
  std::vector<MouseButtonBinding> mouse_bindings{};
  MouseSteering mouse{};
};

struct KeyboardMouseOptions {
  std::string persistent_key{"keyboard-mouse:default"};
  std::string name{"Keyboard + Mouse"};
  // The bindings in effect while no context is selected.
  std::vector<KeyBinding> key_bindings{};
  std::vector<MouseButtonBinding> mouse_bindings{};
  std::vector<KeyboardMouseContext> contexts{};
  bool mouse_to_right_stick{true};
  float mouse_sensitivity_x{900.0f};
  float mouse_sensitivity_y{900.0f};
  bool invert_mouse_x{};
  bool invert_mouse_y{};
  float keyboard_axis_scale{1.0f};
};

[[nodiscard]] KeyboardMouseOptions default_keyboard_mouse_options();

// Names used by input profiles: keys are their US-layout names ("W", "1",
// "Space", "LeftShift", "Numpad8", "F1"), controls their Xbox names
// ("ButtonA", "LeftTrigger", "LeftStickUp"), mouse buttons "Left", "Right",
// "Middle", "X1", "X2". Lookup ignores case.
[[nodiscard]] std::optional<KeyCode> key_from_name(std::string_view name) noexcept;
[[nodiscard]] std::optional<VirtualControl> virtual_control_from_name(std::string_view name) noexcept;
[[nodiscard]] std::optional<MouseButton> mouse_button_from_name(std::string_view name) noexcept;

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
  // Selects options().contexts[name]'s bindings; an empty name returns to the
  // default bindings. False (and no change) for an unknown name. Keys held
  // across the switch are released for the old bindings and pressed for the
  // new ones, so nothing stays latched.
  [[nodiscard]] bool set_context(std::string_view name);
  [[nodiscard]] std::string context() const;
  // True while the active context steers with the mouse, which needs the
  // cursor captured (relative motion, no pointer leaving the window).
  [[nodiscard]] bool wants_mouse_capture() const;

 private:
  [[nodiscard]] GamepadState build_state_locked(bool consume_mouse);
  [[nodiscard]] const KeyboardMouseContext* active_context_locked() const noexcept;
  [[nodiscard]] const std::vector<KeyBinding>& active_keys_locked() const noexcept;
  [[nodiscard]] const std::vector<MouseButtonBinding>& active_mouse_buttons_locked() const noexcept;
  void queue_digital_keystrokes_locked(VirtualControl control, bool pressed);
  [[nodiscard]] static std::uint16_t button_mask(VirtualControl control) noexcept;

  static constexpr NativeDeviceId kDeviceId = 1;
  mutable std::mutex mutex_{};
  KeyboardMouseOptions options_{};
  std::unordered_set<KeyCode> keys_{};
  std::unordered_set<std::uint8_t> mouse_buttons_{};
  float mouse_dx_{};
  float mouse_dy_{};
  // Index into options_.contexts, or -1 for the default bindings.
  int context_index_{-1};
  // Mouse steering: the filtered stick deflection and when it was updated.
  float steer_x_{};
  float steer_y_{};
  std::chrono::steady_clock::time_point steer_time_{};
  std::deque<Keystroke> keystrokes_{};
  bool setup_{};
};

}  // namespace xenon::input
