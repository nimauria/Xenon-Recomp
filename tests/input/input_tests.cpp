#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "xenon/input/system.hpp"

namespace input = xenon::input;

namespace {

class MockDriver final : public input::InputDriver {
 public:
  explicit MockDriver(std::string name) : name_(std::move(name)) {}

  std::string_view name() const noexcept override { return name_; }
  input::Result setup() override {
    ++setup_count;
    return setup_result;
  }
  void shutdown() noexcept override { ++shutdown_count; }

  void enumerate_devices(
      std::vector<input::DriverDeviceInfo>& out_devices) override {
    out_devices = devices;
  }

  input::Result get_state(input::NativeDeviceId device,
                          input::GamepadState& out_state) override {
    ++state_calls;
    for (const auto& item : states) {
      if (item.first == device) {
        out_state = item.second;
        return input::Result::Success;
      }
    }
    return input::Result::DeviceNotConnected;
  }

  input::Result get_capabilities(input::NativeDeviceId device,
                                 input::Capabilities& out_caps) override {
    ++capability_calls;
    for (const auto& info : devices) {
      if (info.native_id == device) {
        out_caps = {};
        out_caps.type = info.type;
        out_caps.subtype = info.subtype;
        if (info.supports_vibration) {
          out_caps.flags |= input::CapabilityVibrationSupported;
        }
        if (info.supports_keystrokes) {
          out_caps.flags |= input::CapabilityKeystrokeSupported;
        }
        return input::Result::Success;
      }
    }
    return input::Result::DeviceNotConnected;
  }

  input::Result set_vibration(input::NativeDeviceId device,
                              const input::Vibration& vibration) override {
    ++vibration_calls;
    last_vibration_device = device;
    last_vibration = vibration;
    return input::Result::Success;
  }

  input::Result get_keystroke(input::NativeDeviceId device,
                              input::Keystroke& out_keystroke) override {
    ++keystroke_calls;
    for (auto& [native, queue] : keystrokes) {
      if (native != device) continue;
      if (queue.empty()) return input::Result::Empty;
      out_keystroke = queue.front();
      queue.pop_front();
      return input::Result::Success;
    }
    return input::Result::Empty;
  }

  input::Result get_power_info(input::NativeDeviceId device,
                               input::PowerInfo& out_power) override {
    ++power_calls;
    for (const auto& [native, power] : powers) {
      if (native == device) {
        out_power = power;
        return input::Result::Success;
      }
    }
    return input::Result::DeviceNotConnected;
  }

  input::Result set_player_indicator(input::NativeDeviceId device,
                                     std::uint8_t player_index) override {
    ++player_indicator_calls;
    last_player_device = device;
    last_player_index = player_index;
    return input::Result::Success;
  }

  void set_state(input::NativeDeviceId device, input::GamepadState state) {
    for (auto& item : states) {
      if (item.first == device) {
        item.second = state;
        return;
      }
    }
    states.emplace_back(device, state);
  }

  void push_keystroke(input::NativeDeviceId device, input::Keystroke key) {
    for (auto& [native, queue] : keystrokes) {
      if (native == device) {
        queue.push_back(key);
        return;
      }
    }
    keystrokes.emplace_back(device, std::deque<input::Keystroke>{key});
  }

  std::string name_;
  input::Result setup_result{input::Result::Success};
  std::vector<input::DriverDeviceInfo> devices{};
  std::vector<std::pair<input::NativeDeviceId, input::GamepadState>> states{};
  std::vector<std::pair<input::NativeDeviceId, std::deque<input::Keystroke>>>
      keystrokes{};
  std::vector<std::pair<input::NativeDeviceId, input::PowerInfo>> powers{};
  int setup_count{};
  int shutdown_count{};
  int state_calls{};
  int capability_calls{};
  int vibration_calls{};
  int keystroke_calls{};
  int power_calls{};
  int player_indicator_calls{};
  input::NativeDeviceId last_vibration_device{};
  input::Vibration last_vibration{};
  input::NativeDeviceId last_player_device{};
  std::uint8_t last_player_index{input::kNoPlayerIndicator};
};

input::DriverDeviceInfo make_pad(input::NativeDeviceId native,
                                 std::string persistent,
                                 std::string name) {
  input::DriverDeviceInfo info{};
  info.native_id = native;
  info.persistent_key = std::move(persistent);
  info.name = std::move(name);
  info.type = input::DeviceType::Gamepad;
  info.subtype = input::DeviceSubtype::Gamepad;
  info.connection = input::ConnectionType::Wired;
  info.supports_vibration = true;
  info.supports_keystrokes = true;
  return info;
}

void test_stable_enumeration_and_auto_assignment() {
  input::InputSystem system;
  auto driver = std::make_unique<MockDriver>("mock");
  auto* mock = driver.get();
  mock->devices = {make_pad(100, "pad-a", "Pad A"),
                   make_pad(200, "pad-b", "Pad B")};
  assert(system.add_driver(std::move(driver)));
  assert(system.setup() == input::Result::Success);

  const auto devices = system.devices();
  assert(devices.size() == 2);
  assert(devices[0].ordinal == 0);
  assert(devices[1].ordinal == 1);
  assert(devices[0].id != input::kInvalidDeviceId);
  assert(devices[1].id != input::kInvalidDeviceId);
  assert(system.device_for_user(0) == devices[0].id);
  assert(system.device_for_user(1) == devices[1].id);
  assert(!system.device_for_user(2).has_value());

  const auto first_id = devices[0].id;
  const auto first_ordinal = devices[0].ordinal;
  mock->devices.erase(mock->devices.begin());
  system.refresh_devices();
  assert(!system.device(first_id)->connected);

  mock->devices.push_back(make_pad(999, "pad-a", "Pad A Reconnected"));
  system.refresh_devices();
  const auto reconnected = system.device(first_id);
  assert(reconnected.has_value());
  assert(reconnected->connected);
  assert(reconnected->ordinal == first_ordinal);
  assert(reconnected->name == "Pad A Reconnected");
}

void test_packet_numbers_change_only_with_state() {
  input::InputSystem system;
  auto driver = std::make_unique<MockDriver>("mock");
  auto* mock = driver.get();
  mock->devices = {make_pad(1, "packet-pad", "Packet Pad")};
  input::GamepadState initial{};
  initial.buttons = input::GamepadButton::A;
  mock->set_state(1, initial);
  assert(system.add_driver(std::move(driver)));
  assert(system.setup() == input::Result::Success);

  input::State first{};
  input::State second{};
  assert(system.get_state(0, first) == input::Result::Success);
  assert(first.packet_number == 1);
  assert(first.gamepad == initial);
  assert(system.get_state(0, second) == input::Result::Success);
  assert(second.packet_number == first.packet_number);

  auto changed = initial;
  changed.thumb_lx = 1234;
  mock->set_state(1, changed);
  input::State third{};
  assert(system.get_state(0, third) == input::Result::Success);
  assert(third.packet_number == first.packet_number + 1);
  assert(third.gamepad == changed);
}

void test_explicit_assignment_and_driver_namespacing() {
  input::InputSystem system;
  auto first = std::make_unique<MockDriver>("first");
  auto second = std::make_unique<MockDriver>("second");
  first->devices = {make_pad(1, "same-key", "First Pad")};
  second->devices = {make_pad(1, "same-key", "Second Pad")};
  assert(system.add_driver(std::move(first)));
  assert(system.add_driver(std::move(second)));
  assert(system.setup() == input::Result::Success);

  const auto devices = system.devices();
  assert(devices.size() == 2);
  assert(devices[0].id != devices[1].id);
  assert(devices[0].persistent_key == devices[1].persistent_key);
  assert(devices[0].driver_name != devices[1].driver_name);

  assert(system.assign_user(3, devices[0].id) == input::Result::Success);
  assert(system.device_for_user(3) == devices[0].id);
  assert(system.clear_user(3) == input::Result::Success);
  assert(!system.device_for_user(3).has_value());
  assert(system.assign_user(9, devices[0].id) == input::Result::BadArguments);
}

void test_capabilities_vibration_and_focus_gating() {
  input::InputSystem system;
  auto driver = std::make_unique<MockDriver>("mock");
  auto* mock = driver.get();
  mock->devices = {make_pad(5, "rumble-pad", "Rumble Pad")};
  assert(system.add_driver(std::move(driver)));
  assert(system.setup() == input::Result::Success);

  input::Capabilities caps{};
  assert(system.get_capabilities(0, 0, caps) == input::Result::Success);
  assert(caps.type == input::DeviceType::Gamepad);
  assert((caps.flags & input::CapabilityVibrationSupported) != 0);
  assert((caps.flags & input::CapabilityKeystrokeSupported) != 0);

  const input::Vibration rumble{30000, 12000};
  assert(system.set_vibration(0, rumble) == input::Result::Success);
  assert(mock->vibration_calls == 1);
  assert(mock->last_vibration == rumble);
  assert(system.set_vibration(0, rumble) == input::Result::Success);
  assert(mock->vibration_calls == 1);  // identical rumble is deduplicated

  system.set_active(false);
  assert(mock->vibration_calls == 2);
  assert(mock->last_vibration == input::Vibration{});
  assert(system.set_vibration(0, rumble) == input::Result::Success);
  assert(mock->vibration_calls == 2);

  input::State neutral{};
  assert(system.get_state(0, neutral) == input::Result::Success);
  assert(neutral.gamepad == input::GamepadState{});
  system.set_active(true);
}

void test_any_user_keystroke_routing() {
  input::InputSystem system;
  auto driver = std::make_unique<MockDriver>("mock");
  auto* mock = driver.get();
  mock->devices = {make_pad(10, "key-a", "Key A"),
                   make_pad(20, "key-b", "Key B")};
  mock->push_keystroke(
      20, input::Keystroke{0x5800, u'B', input::KeystrokeKeyDown, 0, 7});
  assert(system.add_driver(std::move(driver)));
  assert(system.setup() == input::Result::Success);

  input::Keystroke key{};
  assert(system.get_keystroke(input::kAnyUser, 0, key) ==
         input::Result::Success);
  assert(key.virtual_key == 0x5800);
  assert(key.unicode == u'B');
  assert(key.user_index == 1);
  assert(system.get_keystroke(input::kAnyUser, 0, key) == input::Result::Empty);
}

void test_advanced_power_focus_indicators_and_diagnostics() {
  input::InputSystem system;
  auto driver = std::make_unique<MockDriver>("advanced");
  auto* mock = driver.get();
  auto device = make_pad(42, "advanced-pad", "Advanced Pad");
  device.connection = input::ConnectionType::Wireless;
  device.vendor_id = 0x045E;
  device.product_id = 0x028E;
  device.product_version = 0x0114;
  device.serial = "SERIAL-42";
  device.path = "host://controller/42";
  device.supports_power_info = true;
  device.supports_player_indicator = true;
  mock->devices = {device};
  input::GamepadState pressed{};
  pressed.buttons = input::A;
  mock->set_state(42, pressed);
  mock->powers.push_back(
      {42, {input::PowerSource::Battery, input::PowerLevel::Medium, 55, false}});

  assert(system.add_driver(std::move(driver)));
  assert(system.setup() == input::Result::Success);
  assert(mock->player_indicator_calls == 1);
  assert(mock->last_player_device == 42);
  assert(mock->last_player_index == 0);

  const auto devices = system.devices();
  assert(devices.size() == 1);
  assert(devices[0].vendor_id == 0x045E);
  assert(devices[0].product_id == 0x028E);
  assert(devices[0].product_version == 0x0114);
  assert(devices[0].serial == "SERIAL-42");
  assert(devices[0].path == "host://controller/42");
  assert(devices[0].player_indicator == 0);

  input::PowerInfo power{};
  assert(system.get_power_info(0, power) == input::Result::Success);
  assert(power.source == input::PowerSource::Battery);
  assert(power.level == input::PowerLevel::Medium);
  assert(power.percentage == 55);
  const auto cached = system.device(devices[0].id);
  assert(cached && cached->power_valid && cached->power == power);

  const input::Vibration rumble{1234, 5678};
  assert(system.set_vibration(0, rumble) == input::Result::Success);
  const auto before_focus_loss = mock->vibration_calls;
  system.set_focused(false);
  assert(!system.effective_active());
  assert(mock->vibration_calls == before_focus_loss + 1);
  assert(mock->last_vibration == input::Vibration{});

  const auto state_calls_before = mock->state_calls;
  input::State state{};
  assert(system.get_state(0, state) == input::Result::Success);
  assert(state.gamepad == input::GamepadState{});
  assert(mock->state_calls == state_calls_before);

  system.set_background_input_policy(input::BackgroundInputPolicy::Always);
  assert(system.effective_active());
  assert(system.get_state(0, state) == input::Result::Success);
  assert(state.gamepad == pressed);
  assert(mock->state_calls == state_calls_before + 1);

  system.set_active(false);
  assert(!system.effective_active());
  system.set_active(true);
  assert(system.effective_active());
  system.set_background_input_policy(input::BackgroundInputPolicy::ForegroundOnly);
  assert(!system.effective_active());
  system.set_focused(true);
  assert(system.effective_active());

  assert(system.set_player_indicator(0, 3) == input::Result::Success);
  assert(mock->last_player_index == 3);
  assert(system.set_player_indicator(0, 4) == input::Result::BadArguments);

  const auto diagnostics = system.diagnostics();
  assert(diagnostics.setup);
  assert(diagnostics.enabled);
  assert(diagnostics.focused);
  assert(diagnostics.effective_active);
  assert(diagnostics.connected_device_count == 1);
  assert(diagnostics.assigned_user_count == 1);
  assert(diagnostics.devices.size() == 1);
  assert(diagnostics.devices[0].assigned_user == 0);
  assert(diagnostics.devices[0].state_polls >= 2);
  assert(diagnostics.devices[0].power_queries == 1);
  assert(diagnostics.devices[0].vibration_requests >= 1);
}

void test_failed_driver_does_not_break_working_driver() {
  input::InputSystem system;
  auto failed = std::make_unique<MockDriver>("failed");
  failed->setup_result = input::Result::Failed;
  auto good = std::make_unique<MockDriver>("good");
  good->devices = {make_pad(33, "good-pad", "Good Pad")};
  assert(system.add_driver(std::move(failed)));
  assert(system.add_driver(std::move(good)));
  assert(system.setup() == input::Result::Success);
  assert(system.devices().size() == 1);
  assert(system.device_for_user(0).has_value());
}

}  // namespace

int main() {
  test_stable_enumeration_and_auto_assignment();
  test_packet_numbers_change_only_with_state();
  test_explicit_assignment_and_driver_namespacing();
  test_capabilities_vibration_and_focus_gating();
  test_any_user_keystroke_routing();
  test_advanced_power_focus_indicators_and_diagnostics();
  test_failed_driver_does_not_break_working_driver();
  return 0;
}
