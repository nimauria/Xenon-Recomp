#pragma once

#include <array>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xenon/input/driver.hpp"

namespace xenon::input {

struct FlightState {
  // Normalized host axes [-1, 1]. throttle uses [-1, 1], where +1 maps to
  // right trigger and -1 maps to left trigger by default.
  float roll{};
  float pitch{};
  float yaw{};
  float throttle{};
  std::int8_t hat_x{};
  std::int8_t hat_y{};
  std::uint64_t buttons{};
};

struct FlightMapping {
  float roll_scale{1.0f};
  float pitch_scale{-1.0f};
  float yaw_scale{1.0f};
  float throttle_scale{1.0f};
  float deadzone{0.03f};
  // Host button index -> Xbox button mask. Zero means unbound.
  std::array<std::uint16_t, 32> buttons{};

  [[nodiscard]] static FlightMapping ace_combat_style();
};

class FlightInputDriver final : public InputDriver {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "flight"; }
  [[nodiscard]] Result setup() override { setup_ = true; return Result::Success; }
  void shutdown() noexcept override;
  void enumerate_devices(std::vector<DriverDeviceInfo>& out_devices) override;
  [[nodiscard]] Result get_state(NativeDeviceId device, GamepadState& out_state) override;
  [[nodiscard]] Result get_capabilities(NativeDeviceId device, Capabilities& out_caps) override;
  [[nodiscard]] Result set_vibration(NativeDeviceId, const Vibration&) override { return Result::Unsupported; }
  [[nodiscard]] Result get_keystroke(NativeDeviceId, Keystroke&) override { return Result::Empty; }

  // Platform/raw-HID layers feed normalized data here. This intentionally keeps
  // Win32 RawInput, SDL joystick and hidapi details outside the Xbox model.
  [[nodiscard]] Result connect(NativeDeviceId id, std::string persistent_key,
                               std::string name, FlightMapping mapping = FlightMapping::ace_combat_style());
  void disconnect(NativeDeviceId id);
  [[nodiscard]] Result update(NativeDeviceId id, const FlightState& state);

 private:
  struct Device { std::string key; std::string name; FlightMapping mapping; FlightState state; };
  mutable std::mutex mutex_{};
  std::unordered_map<NativeDeviceId, Device> devices_{};
  bool setup_{};
};

}  // namespace xenon::input
