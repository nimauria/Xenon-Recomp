#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "xenon/input/driver.hpp"

namespace xenon::input {

struct XInputHostDevice {
  NativeDeviceId native_id{};
  std::string persistent_key{};
  std::string name{"XInput Controller"};
  DeviceSubtype subtype{DeviceSubtype::Gamepad};
  bool supports_vibration{true};
  bool supports_keystrokes{true};
  bool supports_power_info{true};
};

class XInputHost {
 public:
  virtual ~XInputHost() = default;
  [[nodiscard]] virtual Result setup() = 0;
  virtual void shutdown() noexcept = 0;
  [[nodiscard]] virtual Result enumerate(std::vector<XInputHostDevice>& out) = 0;
  [[nodiscard]] virtual Result get_state(NativeDeviceId device, GamepadState& out) = 0;
  [[nodiscard]] virtual Result get_capabilities(NativeDeviceId device, Capabilities& out) = 0;
  [[nodiscard]] virtual Result set_vibration(NativeDeviceId device, const Vibration& value) = 0;
  [[nodiscard]] virtual Result get_keystroke(NativeDeviceId device, Keystroke& out) = 0;
  [[nodiscard]] virtual Result get_power_info(NativeDeviceId device, PowerInfo& out) = 0;
};

class XInputDriver final : public InputDriver {
 public:
  XInputDriver();
  explicit XInputDriver(std::unique_ptr<XInputHost> host);
  ~XInputDriver() override;

  [[nodiscard]] std::string_view name() const noexcept override { return "xinput"; }
  [[nodiscard]] Result setup() override;
  void shutdown() noexcept override;
  void enumerate_devices(std::vector<DriverDeviceInfo>& out_devices) override;
  [[nodiscard]] Result get_state(NativeDeviceId device, GamepadState& out_state) override;
  [[nodiscard]] Result get_capabilities(NativeDeviceId device, Capabilities& out_caps) override;
  [[nodiscard]] Result set_vibration(NativeDeviceId device, const Vibration& vibration) override;
  [[nodiscard]] Result get_keystroke(NativeDeviceId device, Keystroke& out_keystroke) override;
  [[nodiscard]] Result get_power_info(NativeDeviceId device, PowerInfo& out_power) override;

 private:
  std::unique_ptr<XInputHost> host_;
  bool setup_{};
};

[[nodiscard]] std::unique_ptr<XInputHost> create_win32_xinput_host();
[[nodiscard]] std::unique_ptr<InputDriver> create_xinput_driver();

}  // namespace xenon::input
