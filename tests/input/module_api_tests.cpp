#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "xenon/input/module_api.hpp"
#include "xenon/input/module_api_provider.hpp"
#include "xenon/input/driver.hpp"
#include "xenon/input/system.hpp"

namespace input = xenon::input;

namespace {
class Driver final : public input::InputDriver {
 public:
  std::string_view name() const noexcept override { return "module-api-test"; }
  input::Result setup() override { return input::Result::Success; }
  void shutdown() noexcept override {}
  void enumerate_devices(std::vector<input::DriverDeviceInfo>& out) override {
    input::DriverDeviceInfo info{};
    info.native_id = 7;
    info.persistent_key = "primary";
    info.name = "Module API Pad";
    info.type = input::DeviceType::Gamepad;
    info.subtype = input::DeviceSubtype::FlightStick;
    info.connection = input::ConnectionType::Wireless;
    info.vendor_id = 0x1234;
    info.product_id = 0x5678;
    info.supports_vibration = true;
    info.supports_power_info = true;
    out = {info};
  }
  input::Result get_state(input::NativeDeviceId id, input::GamepadState& out) override {
    if (id != 7) return input::Result::DeviceNotConnected;
    out.buttons = input::A | input::DpadUp;
    out.left_trigger = 90;
    out.thumb_lx = 1111;
    return input::Result::Success;
  }
  input::Result get_capabilities(input::NativeDeviceId id, input::Capabilities& out) override {
    if (id != 7) return input::Result::DeviceNotConnected;
    out = {};
    out.type = input::DeviceType::Gamepad;
    out.subtype = input::DeviceSubtype::FlightStick;
    out.flags = input::CapabilityVibrationSupported;
    return input::Result::Success;
  }
  input::Result set_vibration(input::NativeDeviceId id, const input::Vibration& value) override {
    if (id != 7) return input::Result::DeviceNotConnected;
    last = value;
    return input::Result::Success;
  }
  input::Result get_keystroke(input::NativeDeviceId id, input::Keystroke& out) override {
    if (id != 7) return input::Result::DeviceNotConnected;
    if (key_sent) return input::Result::Empty;
    out.virtual_key = 0x5800;
    out.unicode = u'Q';
    out.flags = input::KeystrokeKeyDown;
    key_sent = true;
    return input::Result::Success;
  }
  input::Result get_power_info(input::NativeDeviceId id, input::PowerInfo& out) override {
    if (id != 7) return input::Result::DeviceNotConnected;
    out = {input::PowerSource::Battery, input::PowerLevel::Medium, 55, false};
    return input::Result::Success;
  }
  input::Vibration last{};
  bool key_sent{};
};

void test_module_api_contract() {
  input::InputSystem system;
  auto driver = std::make_unique<Driver>();
  auto* raw = driver.get();
  assert(system.add_driver(std::move(driver)));
  assert(system.setup() == input::Result::Success);

  input::module_api::Provider provider(system);
  const auto* api = provider.api_ptr();
  assert(api);
  assert(api->abi_version == input::module_api::kAbiVersion);
  assert(api->struct_size == sizeof(input::module_api::ApiV1));
  assert((api->feature_flags & input::module_api::FeatureFlightDevices) != 0);
  assert((api->feature_flags & input::module_api::FeatureMultiSource) != 0);

  input::module_api::StateV1 state{};
  assert(api->get_state(api->context, 0, &state) == static_cast<int>(input::module_api::ResultV1::Success));
  assert((state.gamepad.buttons & input::A) != 0);
  assert(state.gamepad.left_trigger == 90);

  input::module_api::CapabilitiesV1 caps{};
  assert(api->get_capabilities(api->context, 0, &caps) == static_cast<int>(input::module_api::ResultV1::Success));
  assert(caps.subtype == static_cast<std::uint8_t>(input::DeviceSubtype::FlightStick));

  assert(api->set_vibration(api->context, 0, 123, 456) == static_cast<int>(input::module_api::ResultV1::Success));
  assert(raw->last.left_motor_speed == 123 && raw->last.right_motor_speed == 456);

  input::module_api::KeystrokeV1 key{};
  assert(api->get_keystroke(api->context, 0, 0, &key) ==
         static_cast<int>(input::module_api::ResultV1::Success));
  assert(key.virtual_key == 0x5800);
  assert(key.unicode == static_cast<std::uint16_t>(u'Q'));
  assert(key.flags == input::KeystrokeKeyDown);
  assert(api->get_keystroke(api->context, 0, 0, &key) ==
         static_cast<int>(input::module_api::ResultV1::Empty));

  input::module_api::PowerInfoV1 power{};
  assert(api->get_power(api->context, 0, &power) == static_cast<int>(input::module_api::ResultV1::Success));
  assert(power.percentage == 55);

  input::module_api::DeviceInfoV1 device{};
  assert(api->get_primary_device(api->context, 0, &device) == static_cast<int>(input::module_api::ResultV1::Success));
  assert(device.vendor_id == 0x1234);
  assert(device.product_id == 0x5678);
  assert(std::string(device.name) == "Module API Pad");
  assert(api->get_source_count(api->context, 0) == 1);
  assert(api->get_source_device(api->context, 0, 0, &device) == static_cast<int>(input::module_api::ResultV1::Success));
}
}

int main() {
  test_module_api_contract();
  return 0;
}
