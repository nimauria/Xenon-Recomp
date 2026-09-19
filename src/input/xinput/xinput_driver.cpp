#include "xenon/input/xinput_driver.hpp"

namespace xenon::input {

XInputDriver::XInputDriver() : XInputDriver(create_win32_xinput_host()) {}
XInputDriver::XInputDriver(std::unique_ptr<XInputHost> host) : host_(std::move(host)) {}
XInputDriver::~XInputDriver() { shutdown(); }

Result XInputDriver::setup() {
  if (setup_) return Result::Success;
  if (!host_) return Result::Unsupported;
  const auto result = host_->setup();
  setup_ = result == Result::Success;
  return result;
}
void XInputDriver::shutdown() noexcept {
  if (setup_ && host_) host_->shutdown();
  setup_ = false;
}
void XInputDriver::enumerate_devices(std::vector<DriverDeviceInfo>& out) {
  out.clear();
  if (!setup_ || !host_) return;
  std::vector<XInputHostDevice> devices;
  if (host_->enumerate(devices) != Result::Success) return;
  for (const auto& device : devices) {
    DriverDeviceInfo info{};
    info.native_id = device.native_id;
    info.persistent_key = device.persistent_key;
    info.name = device.name;
    info.type = DeviceType::Gamepad;
    info.subtype = device.subtype;
    info.connection = ConnectionType::Unknown;
    info.supports_vibration = device.supports_vibration;
    info.supports_keystrokes = device.supports_keystrokes;
    info.supports_power_info = device.supports_power_info;
    out.push_back(std::move(info));
  }
}
Result XInputDriver::get_state(NativeDeviceId id, GamepadState& out) {
  out = {};
  return setup_ && host_ ? host_->get_state(id, out) : Result::Failed;
}
Result XInputDriver::get_capabilities(NativeDeviceId id, Capabilities& out) {
  out = {};
  return setup_ && host_ ? host_->get_capabilities(id, out) : Result::Failed;
}
Result XInputDriver::set_vibration(NativeDeviceId id, const Vibration& value) {
  return setup_ && host_ ? host_->set_vibration(id, value) : Result::Failed;
}
Result XInputDriver::get_keystroke(NativeDeviceId id, Keystroke& out) {
  out = {};
  return setup_ && host_ ? host_->get_keystroke(id, out) : Result::Failed;
}
Result XInputDriver::get_power_info(NativeDeviceId id, PowerInfo& out) {
  out = {};
  return setup_ && host_ ? host_->get_power_info(id, out) : Result::Failed;
}
std::unique_ptr<InputDriver> create_xinput_driver() {
  return std::make_unique<XInputDriver>();
}

#if !defined(_WIN32)
std::unique_ptr<XInputHost> create_win32_xinput_host() { return {}; }
#endif

}  // namespace xenon::input
