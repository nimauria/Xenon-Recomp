#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenon/input/sdl_driver.hpp"
#include "xenon/input/system.hpp"

namespace input = xenon::input;

namespace {

class TempDirectory {
 public:
  TempDirectory() {
    const auto value = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("xenon_input_sdl_" + std::to_string(value));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_{};
};

class FakeSdlHost final : public input::SdlHost {
 public:
  input::Result setup() override {
    ++setup_calls;
    setup_active = setup_result == input::Result::Success;
    return setup_result;
  }

  void shutdown() noexcept override {
    ++shutdown_calls;
    setup_active = false;
  }

  int load_mappings_file(std::string_view path) override {
    ++mapping_calls;
    last_mapping_path = std::string(path);
    return mapping_result;
  }

  input::Result enumerate(
      std::vector<input::SdlHostDevice>& out_devices) override {
    ++enumerate_calls;
    out_devices = devices;
    return setup_active ? input::Result::Success : input::Result::Failed;
  }

  input::Result get_state(input::NativeDeviceId device,
                          input::GamepadState& out_state) override {
    ++state_calls;
    const auto it = states.find(device);
    if (it == states.end()) return input::Result::DeviceNotConnected;
    out_state = it->second;
    return input::Result::Success;
  }

  input::Result rumble(input::NativeDeviceId device,
                       const input::Vibration& vibration,
                       std::uint32_t duration_ms) override {
    ++rumble_calls;
    last_rumble_device = device;
    last_rumble = vibration;
    last_rumble_duration = duration_ms;
    return states.contains(device) ? input::Result::Success
                                   : input::Result::DeviceNotConnected;
  }

  input::Result get_power_info(input::NativeDeviceId device,
                               input::PowerInfo& out_power) override {
    ++power_calls;
    const auto it = power.find(device);
    if (it == power.end()) return input::Result::DeviceNotConnected;
    out_power = it->second;
    return input::Result::Success;
  }

  input::Result set_player_index(input::NativeDeviceId device,
                                 std::uint8_t player_index) override {
    ++player_index_calls;
    last_player_device = device;
    last_player_index = player_index;
    return states.contains(device) ? input::Result::Success
                                   : input::Result::DeviceNotConnected;
  }

  std::uint64_t now_millis() const noexcept override { return now; }

  input::Result setup_result{input::Result::Success};
  std::vector<input::SdlHostDevice> devices{};
  std::unordered_map<input::NativeDeviceId, input::GamepadState> states{};
  std::unordered_map<input::NativeDeviceId, input::PowerInfo> power{};
  std::uint64_t now{};
  int mapping_result{3};
  int setup_calls{};
  int shutdown_calls{};
  int mapping_calls{};
  int enumerate_calls{};
  int state_calls{};
  int rumble_calls{};
  int power_calls{};
  int player_index_calls{};
  bool setup_active{};
  std::string last_mapping_path{};
  input::NativeDeviceId last_rumble_device{};
  input::Vibration last_rumble{};
  std::uint32_t last_rumble_duration{};
  input::NativeDeviceId last_player_device{};
  std::uint8_t last_player_index{input::kNoPlayerIndicator};
};

input::SdlHostDevice pad(input::NativeDeviceId native_id,
                         std::string persistent_key,
                         std::string name,
                         bool vibration = true) {
  input::SdlHostDevice result{};
  result.native_id = native_id;
  result.persistent_key = std::move(persistent_key);
  result.name = std::move(name);
  result.subtype = input::DeviceSubtype::Gamepad;
  result.connection = input::ConnectionType::Wireless;
  result.vendor_id = 0x045E;
  result.product_id = 0x028E;
  result.product_version = 0x0114;
  result.serial = result.persistent_key + "-serial";
  result.supports_vibration = vibration;
  result.supports_power_info = true;
  result.supports_player_indicator = true;
  return result;
}

void test_setup_mappings_and_enumeration() {
  TempDirectory temp;
  const auto mappings = temp.path() / "gamecontrollerdb.txt";
  {
    std::ofstream file(mappings);
    file << "fake mapping\n";
  }

  auto host = std::make_unique<FakeSdlHost>();
  auto* fake = host.get();
  fake->devices = {pad(22, "guid-b", "Pad B"),
                   pad(11, "guid-a", "Pad A", false)};
  input::SdlInputOptions options{};
  options.mappings_file = mappings.string();
  input::SdlInputDriver driver(std::move(host), options);
  assert(driver.available());
  assert(driver.setup() == input::Result::Success);
  assert(fake->setup_calls == 1);
  assert(fake->mapping_calls == 1);
  assert(driver.loaded_mapping_count() == 3);

  std::vector<input::DriverDeviceInfo> devices;
  driver.enumerate_devices(devices);
  assert(devices.size() == 2);
  // The driver deliberately emits deterministic persistent-key order, not SDL
  // joystick-index order.
  assert(devices[0].persistent_key == "guid-a");
  assert(devices[0].native_id == 11);
  assert(!devices[0].supports_vibration);
  assert(devices[0].supports_keystrokes);
  assert(devices[1].persistent_key == "guid-b");
  assert(devices[1].connection == input::ConnectionType::Wireless);

  driver.shutdown();
  assert(fake->shutdown_calls == 1);
}

void test_state_capabilities_and_rumble() {
  auto host = std::make_unique<FakeSdlHost>();
  auto* fake = host.get();
  fake->devices = {pad(7, "state-pad", "State Pad")};
  input::GamepadState expected{};
  expected.buttons = input::A | input::LeftShoulder;
  expected.left_trigger = 99;
  expected.thumb_lx = -12345;
  expected.thumb_ry = 23456;
  fake->states[7] = expected;

  input::SdlInputOptions options{};
  options.load_mappings_if_present = false;
  options.rumble_duration_ms = 4321;
  input::SdlInputDriver driver(std::move(host), options);
  assert(driver.setup() == input::Result::Success);

  std::vector<input::DriverDeviceInfo> devices;
  driver.enumerate_devices(devices);
  input::GamepadState actual{};
  assert(driver.get_state(7, actual) == input::Result::Success);
  assert(actual == expected);

  input::Capabilities caps{};
  assert(driver.get_capabilities(7, caps) == input::Result::Success);
  assert((caps.flags & input::CapabilityVibrationSupported) != 0);
  assert((caps.flags & input::CapabilityKeystrokeSupported) != 0);
  assert(caps.gamepad.left_trigger == 0xFF);

  const input::Vibration vibration{50000, 12000};
  assert(driver.set_vibration(7, vibration) == input::Result::Success);
  assert(fake->rumble_calls == 1);
  assert(fake->last_rumble_device == 7);
  assert(fake->last_rumble == vibration);
  assert(fake->last_rumble_duration == 4321);
}

void test_unsupported_rumble_is_explicit() {
  auto host = std::make_unique<FakeSdlHost>();
  auto* fake = host.get();
  fake->devices = {pad(3, "quiet-pad", "Quiet Pad", false)};
  fake->states[3] = {};
  input::SdlInputOptions options{};
  options.load_mappings_if_present = false;
  input::SdlInputDriver driver(std::move(host), options);
  assert(driver.setup() == input::Result::Success);
  std::vector<input::DriverDeviceInfo> devices;
  driver.enumerate_devices(devices);

  assert(driver.set_vibration(3, {1, 1}) == input::Result::Unsupported);
  assert(fake->rumble_calls == 0);
  assert(driver.set_vibration(3, {}) == input::Result::Success);
  assert(fake->rumble_calls == 0);
}

void test_button_analog_and_repeat_keystrokes() {
  auto host = std::make_unique<FakeSdlHost>();
  auto* fake = host.get();
  fake->devices = {pad(1, "key-pad", "Key Pad")};
  fake->states[1] = {};
  input::SdlInputOptions options{};
  options.load_mappings_if_present = false;
  input::SdlInputDriver driver(std::move(host), options);
  assert(driver.setup() == input::Result::Success);
  std::vector<input::DriverDeviceInfo> devices;
  driver.enumerate_devices(devices);

  fake->states[1].buttons = input::A;
  input::Keystroke key{};
  assert(driver.get_keystroke(1, key) == input::Result::Success);
  assert(key.virtual_key == 0x5800);
  assert(key.flags == input::KeystrokeKeyDown);

  fake->now = 399;
  assert(driver.get_keystroke(1, key) == input::Result::Empty);
  fake->now = 400;
  assert(driver.get_keystroke(1, key) == input::Result::Success);
  assert(key.virtual_key == 0x5800);
  assert(key.flags == (input::KeystrokeKeyDown | input::KeystrokeRepeat));

  fake->states[1].buttons = 0;
  assert(driver.get_keystroke(1, key) == input::Result::Success);
  assert(key.virtual_key == 0x5800);
  assert(key.flags == input::KeystrokeKeyUp);

  fake->states[1].left_trigger = 0x20;
  assert(driver.get_keystroke(1, key) == input::Result::Success);
  assert(key.virtual_key == 0x5806);

  fake->states[1].left_trigger = 0;
  fake->states[1].thumb_lx = -25000;
  fake->states[1].thumb_ly = 25000;
  // Release the trigger first, then the new diagonal is reported.
  assert(driver.get_keystroke(1, key) == input::Result::Success);
  assert(key.virtual_key == 0x5806 && key.flags == input::KeystrokeKeyUp);
  assert(driver.get_keystroke(1, key) == input::Result::Success);
  assert(key.virtual_key == 0x5824);  // left thumb up-left
}

void test_hotplug_reconnect_preserves_core_identity() {
  input::InputSystem system;
  auto host = std::make_unique<FakeSdlHost>();
  auto* fake = host.get();
  fake->devices = {pad(10, "serial-abc", "First Connection")};
  fake->states[10] = {};
  input::SdlInputOptions options{};
  options.load_mappings_if_present = false;
  assert(system.add_driver(
      std::make_unique<input::SdlInputDriver>(std::move(host), options)));
  assert(system.setup() == input::Result::Success);

  const auto first_devices = system.devices();
  assert(first_devices.size() == 1);
  const auto stable_id = first_devices[0].id;
  const auto stable_ordinal = first_devices[0].ordinal;

  fake->devices.clear();
  fake->states.clear();
  system.refresh_devices();
  assert(!system.device(stable_id)->connected);

  fake->devices = {pad(99, "serial-abc", "Reconnected")};
  fake->states[99] = {};
  system.refresh_devices();
  const auto reconnected = system.device(stable_id);
  assert(reconnected.has_value());
  assert(reconnected->connected);
  assert(reconnected->ordinal == stable_ordinal);
  assert(reconnected->name == "Reconnected");
}

void test_power_player_indicator_and_metadata() {
  auto host = std::make_unique<FakeSdlHost>();
  auto* fake = host.get();
  fake->devices = {pad(8, "advanced-pad", "Advanced Pad")};
  fake->states[8] = {};
  fake->power[8] = {input::PowerSource::Battery, input::PowerLevel::Medium,
                    0xFFu, false};
  input::SdlInputOptions options{};
  options.load_mappings_if_present = false;
  input::SdlInputDriver driver(std::move(host), options);
  assert(driver.setup() == input::Result::Success);

  std::vector<input::DriverDeviceInfo> devices;
  driver.enumerate_devices(devices);
  assert(devices.size() == 1);
  assert(devices[0].vendor_id == 0x045E);
  assert(devices[0].product_id == 0x028E);
  assert(devices[0].product_version == 0x0114);
  assert(devices[0].serial == "advanced-pad-serial");
  assert(devices[0].supports_power_info);
  assert(devices[0].supports_player_indicator);

  input::PowerInfo power{};
  assert(driver.get_power_info(8, power) == input::Result::Success);
  assert(power.source == input::PowerSource::Battery);
  assert(power.level == input::PowerLevel::Medium);
  assert(fake->power_calls == 1);

  assert(driver.set_player_indicator(8, 2) == input::Result::Success);
  assert(fake->player_index_calls == 1);
  assert(fake->last_player_device == 8);
  assert(fake->last_player_index == 2);
}

void test_missing_real_sdl_backend_fails_truthfully_when_unavailable() {
#if !defined(XENON_INPUT_HAS_SDL2)
  input::SdlInputOptions options{};
  options.load_mappings_if_present = false;
  input::SdlInputDriver driver(options);
  assert(!driver.available());
  assert(driver.setup() == input::Result::Unsupported);
#endif
}

}  // namespace

int main() {
  test_setup_mappings_and_enumeration();
  test_state_capabilities_and_rumble();
  test_unsupported_rumble_is_explicit();
  test_button_analog_and_repeat_keystrokes();
  test_hotplug_reconnect_preserves_core_identity();
  test_power_player_indicator_and_metadata();
  test_missing_real_sdl_backend_fails_truthfully_when_unavailable();
  return 0;
}
