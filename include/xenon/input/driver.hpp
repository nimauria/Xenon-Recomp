#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "xenon/input/types.hpp"

namespace xenon::input {

class InputDriver {
 public:
  virtual ~InputDriver() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual Result setup() = 0;
  virtual void shutdown() noexcept = 0;

  // Drivers enumerate only currently connected devices. The core assigns
  // cross-driver DeviceId/ordinal values and never recycles them.
  virtual void enumerate_devices(std::vector<DriverDeviceInfo>& out_devices) = 0;

  [[nodiscard]] virtual Result get_state(NativeDeviceId device,
                                         GamepadState& out_state) = 0;
  [[nodiscard]] virtual Result get_capabilities(NativeDeviceId device,
                                                Capabilities& out_caps) = 0;
  [[nodiscard]] virtual Result set_vibration(NativeDeviceId device,
                                             const Vibration& vibration) = 0;
  [[nodiscard]] virtual Result get_keystroke(NativeDeviceId device,
                                             Keystroke& out_keystroke) = 0;

  // Optional advanced-device operations. Backends that cannot expose these
  // features report Unsupported rather than fabricating data.
  [[nodiscard]] virtual Result get_power_info(NativeDeviceId device,
                                              PowerInfo& out_power) {
    static_cast<void>(device);
    out_power = {};
    return Result::Unsupported;
  }
  [[nodiscard]] virtual Result set_player_indicator(
      NativeDeviceId device, std::uint8_t player_index) {
    static_cast<void>(device);
    static_cast<void>(player_index);
    return Result::Unsupported;
  }
};

}  // namespace xenon::input
