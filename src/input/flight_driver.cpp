#include "xenon/input/flight_driver.hpp"

#include <algorithm>
#include <cmath>

namespace xenon::input {
namespace {

float apply_deadzone(float value, float deadzone) noexcept {
  if (std::abs(value) < deadzone) return 0.0f;
  return std::clamp(value, -1.0f, 1.0f);
}

std::int16_t to_axis(float value) noexcept {
  const float clamped = std::clamp(value, -1.0f, 1.0f);
  const float scale = clamped < 0.0f ? 32768.0f : 32767.0f;
  return static_cast<std::int16_t>(std::lround(clamped * scale));
}

std::uint8_t to_trigger(float value) noexcept {
  return static_cast<std::uint8_t>(
      std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

}  // namespace

FlightMapping FlightMapping::ace_combat_style() {
  FlightMapping mapping{};
  mapping.buttons[0] = A;
  mapping.buttons[1] = B;
  mapping.buttons[2] = X;
  mapping.buttons[3] = Y;
  mapping.buttons[4] = LeftShoulder;
  mapping.buttons[5] = RightShoulder;
  mapping.buttons[6] = Back;
  mapping.buttons[7] = Start;
  mapping.buttons[8] = LeftThumb;
  return mapping;
}

void FlightInputDriver::shutdown() noexcept {
  std::scoped_lock lock(mutex_);
  devices_.clear();
  setup_ = false;
}

Result FlightInputDriver::connect(NativeDeviceId id, std::string key,
                                  std::string name, FlightMapping mapping) {
  if (id == 0 || key.empty()) return Result::BadArguments;
  std::scoped_lock lock(mutex_);
  devices_[id] = {std::move(key), std::move(name), mapping, {}};
  return Result::Success;
}

void FlightInputDriver::disconnect(NativeDeviceId id) {
  std::scoped_lock lock(mutex_);
  devices_.erase(id);
}

Result FlightInputDriver::update(NativeDeviceId id, const FlightState& state) {
  std::scoped_lock lock(mutex_);
  const auto it = devices_.find(id);
  if (it == devices_.end()) return Result::DeviceNotConnected;
  it->second.state = state;
  return Result::Success;
}

void FlightInputDriver::enumerate_devices(
    std::vector<DriverDeviceInfo>& out_devices) {
  out_devices.clear();
  if (!setup_) return;

  std::scoped_lock lock(mutex_);
  out_devices.reserve(devices_.size());
  for (const auto& [id, device] : devices_) {
    DriverDeviceInfo info{};
    info.native_id = id;
    info.persistent_key = device.key;
    info.name = device.name;
    info.type = DeviceType::Gamepad;
    info.subtype = DeviceSubtype::FlightStick;
    info.connection = ConnectionType::Unknown;
    out_devices.push_back(std::move(info));
  }
}

Result FlightInputDriver::get_state(NativeDeviceId id, GamepadState& out) {
  out = {};
  if (!setup_) return Result::Failed;

  std::scoped_lock lock(mutex_);
  const auto it = devices_.find(id);
  if (it == devices_.end()) return Result::DeviceNotConnected;

  const auto& state = it->second.state;
  const auto& mapping = it->second.mapping;
  out.thumb_lx = to_axis(
      apply_deadzone(state.roll * mapping.roll_scale, mapping.deadzone));
  out.thumb_ly = to_axis(
      apply_deadzone(state.pitch * mapping.pitch_scale, mapping.deadzone));
  out.thumb_rx = to_axis(
      apply_deadzone(state.yaw * mapping.yaw_scale, mapping.deadzone));

  const float throttle =
      std::clamp(state.throttle * mapping.throttle_scale, -1.0f, 1.0f);
  out.right_trigger = to_trigger(std::max(0.0f, throttle));
  out.left_trigger = to_trigger(std::max(0.0f, -throttle));

  if (state.hat_y > 0) out.buttons |= DpadUp;
  if (state.hat_y < 0) out.buttons |= DpadDown;
  if (state.hat_x < 0) out.buttons |= DpadLeft;
  if (state.hat_x > 0) out.buttons |= DpadRight;

  for (std::size_t button = 0; button < mapping.buttons.size(); ++button) {
    if ((state.buttons & (std::uint64_t{1} << button)) != 0) {
      out.buttons |= mapping.buttons[button];
    }
  }
  return Result::Success;
}

Result FlightInputDriver::get_capabilities(NativeDeviceId id,
                                           Capabilities& out) {
  std::scoped_lock lock(mutex_);
  if (!devices_.contains(id)) return Result::DeviceNotConnected;

  out = {};
  out.type = DeviceType::Gamepad;
  out.subtype = DeviceSubtype::FlightStick;
  out.gamepad.buttons = 0xFFFF;
  out.gamepad.left_trigger = 0xFF;
  out.gamepad.right_trigger = 0xFF;
  out.gamepad.thumb_lx = 0x7FFF;
  out.gamepad.thumb_ly = 0x7FFF;
  out.gamepad.thumb_rx = 0x7FFF;
  out.gamepad.thumb_ry = 0x7FFF;
  return Result::Success;
}

}  // namespace xenon::input
