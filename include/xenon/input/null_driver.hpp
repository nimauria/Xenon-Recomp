#pragma once

#include "xenon/input/driver.hpp"

namespace xenon::input {

class NullInputDriver final : public InputDriver {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "null"; }
  [[nodiscard]] Result setup() override { return Result::Success; }
  void shutdown() noexcept override {}
  void enumerate_devices(std::vector<DriverDeviceInfo>& out_devices) override {
    out_devices.clear();
  }
  [[nodiscard]] Result get_state(NativeDeviceId, GamepadState&) override {
    return Result::DeviceNotConnected;
  }
  [[nodiscard]] Result get_capabilities(NativeDeviceId, Capabilities&) override {
    return Result::DeviceNotConnected;
  }
  [[nodiscard]] Result set_vibration(NativeDeviceId,
                                     const Vibration&) override {
    return Result::DeviceNotConnected;
  }
  [[nodiscard]] Result get_keystroke(NativeDeviceId, Keystroke&) override {
    return Result::DeviceNotConnected;
  }
};

}  // namespace xenon::input
