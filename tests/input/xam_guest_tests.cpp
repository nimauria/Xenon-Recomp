#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <string_view>
#include <vector>

#include "xenon/input/xam_guest.hpp"
#include "xenon/cpu/registry_runtime.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/fault.hpp"

namespace input = xenon::input;
namespace xam = xenon::input::xam;
namespace guest = xenon::input::xam::guest;
namespace memory = xenon::memory;

namespace {

class GuestTestDriver final : public input::InputDriver {
 public:
  std::string_view name() const noexcept override { return "guest-test"; }
  input::Result setup() override { return input::Result::Success; }
  void shutdown() noexcept override {}

  void enumerate_devices(std::vector<input::DriverDeviceInfo>& out) override {
    input::DriverDeviceInfo info{};
    info.native_id = 0x55;
    info.persistent_key = "wireless-flight-pad";
    info.name = "Guest ABI Pad";
    info.type = input::DeviceType::Gamepad;
    info.subtype = input::DeviceSubtype::FlightStick;
    info.connection = input::ConnectionType::Wireless;
    info.supports_vibration = true;
    info.supports_keystrokes = true;
    out = {info};
  }

  input::Result get_state(input::NativeDeviceId device,
                          input::GamepadState& out) override {
    if (device != 0x55) return input::Result::DeviceNotConnected;
    out = state;
    return input::Result::Success;
  }

  input::Result get_capabilities(input::NativeDeviceId device,
                                 input::Capabilities& out) override {
    if (device != 0x55) return input::Result::DeviceNotConnected;
    out = {};
    out.type = input::DeviceType::Gamepad;
    out.subtype = input::DeviceSubtype::FlightStick;
    out.flags = input::CapabilityVibrationSupported |
                input::CapabilityKeystrokeSupported;
    out.gamepad.buttons = 0xF3FFu;
    out.gamepad.left_trigger = 0xFFu;
    out.gamepad.right_trigger = 0xFFu;
    out.gamepad.thumb_lx = -1;
    out.gamepad.thumb_ly = -1;
    out.gamepad.thumb_rx = -1;
    out.gamepad.thumb_ry = -1;
    out.vibration = {0xFFFFu, 0xFFFFu};
    return input::Result::Success;
  }

  input::Result set_vibration(input::NativeDeviceId device,
                              const input::Vibration& value) override {
    if (device != 0x55) return input::Result::DeviceNotConnected;
    last_vibration = value;
    ++vibration_calls;
    return input::Result::Success;
  }

  input::Result get_keystroke(input::NativeDeviceId device,
                              input::Keystroke& out) override {
    if (device != 0x55) return input::Result::DeviceNotConnected;
    if (keys.empty()) return input::Result::Empty;
    out = keys.front();
    keys.pop_front();
    return input::Result::Success;
  }

  input::GamepadState state{};
  input::Vibration last_vibration{};
  int vibration_calls{};
  std::deque<input::Keystroke> keys{};
};

struct Fixture {
  input::InputSystem system{};
  GuestTestDriver* driver{};
  guest::GuestInputBridge bridge;
  memory::AddressSpace mem{};

  Fixture() : bridge(system) {
    auto owned = std::make_unique<GuestTestDriver>();
    driver = owned.get();
    driver->state.buttons = input::A | input::DpadUp;
    driver->state.left_trigger = 0x7Fu;
    driver->state.right_trigger = 0x80u;
    driver->state.thumb_lx = -12345;
    driver->state.thumb_ly = 23456;
    driver->state.thumb_rx = -30000;
    driver->state.thumb_ry = 30001;
    assert(system.add_driver(std::move(owned)));
    assert(system.setup() == input::Result::Success);
    assert(mem.initialize());
    assert(mem.commit_fixed(0x00100000u, 0x1000u, memory::kReadWrite));
  }
};

void test_guest_layout_and_state_marshalling() {
  Fixture f;
  constexpr xenon::cpu::GuestAddress kState = 0x00100040u;
  assert(f.bridge.get_state(f.mem, 0, xam::kFlagGamepad, kState) ==
         xam::result::Success);
  assert(f.mem.read32_be(kState) == 1u);
  assert(f.mem.read16_be(kState + 4u) == (input::A | input::DpadUp));
  assert(f.mem.read8(kState + 6u) == 0x7Fu);
  assert(f.mem.read8(kState + 7u) == 0x80u);
  assert(static_cast<std::int16_t>(f.mem.read16_be(kState + 8u)) == -12345);
  assert(static_cast<std::int16_t>(f.mem.read16_be(kState + 10u)) == 23456);
  assert(static_cast<std::int16_t>(f.mem.read16_be(kState + 12u)) == -30000);
  assert(static_cast<std::int16_t>(f.mem.read16_be(kState + 14u)) == 30001);
  static_assert(guest::layout::StateSize == 16);
}

void test_capabilities_and_vibration_marshalling() {
  Fixture f;
  constexpr xenon::cpu::GuestAddress kCaps = 0x00100080u;
  constexpr xenon::cpu::GuestAddress kVibration = 0x001000C0u;
  assert(f.bridge.get_capabilities(f.mem, 0, xam::kFlagGamepad, kCaps) ==
         xam::result::Success);
  assert(f.mem.read8(kCaps + guest::layout::capabilities::Type) == 0x01u);
  assert(f.mem.read8(kCaps + guest::layout::capabilities::Subtype) == 0x04u);
  const auto flags = f.mem.read16_be(kCaps + guest::layout::capabilities::Flags);
  assert((flags & guest::capability_flag::ForceFeedbackSupported) != 0);
  assert((flags & guest::capability_flag::Wireless) != 0);
  assert(f.mem.read16_be(kCaps + guest::layout::capabilities::Vibration) ==
         0xFFFFu);

  f.mem.write16_be(kVibration, 0x1234u);
  f.mem.write16_be(kVibration + 2u, 0xABCDu);
  assert(f.bridge.set_state(f.mem, 0, 0, kVibration) == xam::result::Success);
  assert(f.driver->vibration_calls == 1);
  assert(f.driver->last_vibration.left_motor_speed == 0x1234u);
  assert(f.driver->last_vibration.right_motor_speed == 0xABCDu);
}

void test_keystroke_and_ex_marshalling() {
  Fixture f;
  constexpr xenon::cpu::GuestAddress kKey = 0x00100100u;
  constexpr xenon::cpu::GuestAddress kUser = 0x00100120u;
  f.driver->keys.push_back(
      {0x5800u, u'Q', input::KeystrokeKeyDown, 0, 0x22u});
  assert(f.bridge.get_keystroke(f.mem, 0, xam::kFlagGamepad, kKey) ==
         xam::result::Success);
  assert(f.mem.read16_be(kKey) == 0x5800u);
  assert(f.mem.read16_be(kKey + 2u) == static_cast<std::uint16_t>(u'Q'));
  assert(f.mem.read16_be(kKey + 4u) == input::KeystrokeKeyDown);
  assert(f.mem.read8(kKey + 6u) == 0u);
  assert(f.mem.read8(kKey + 7u) == 0x22u);

  f.driver->keys.push_back(
      {0x5801u, 0, input::KeystrokeKeyUp, 0, 0x33u});
  f.mem.write32_be(kUser, xam::kUserIndexAny);
  assert(f.bridge.get_keystroke_ex(f.mem, kUser, xam::kFlagAnyUser, kKey) ==
         xam::result::Success);
  assert(f.mem.read32_be(kUser) == 0u);
  assert(f.mem.read16_be(kKey) == 0x5801u);
  assert(f.mem.read16_be(kKey + 4u) == input::KeystrokeKeyUp);
}

void test_ppc_ordinal_dispatch() {
  Fixture f;
  constexpr xenon::cpu::GuestAddress kState = 0x00100180u;
  xenon::cpu::CpuState cpu{};
  cpu.gpr[3] = 0;
  cpu.gpr[4] = xam::kFlagGamepad;
  cpu.gpr[5] = kState;
  assert(f.bridge.dispatch(guest::ordinal::XamInputGetState, cpu, f.mem));
  assert(cpu.gpr[3] == xam::result::Success);
  assert(f.mem.read16_be(kState + 4u) == (input::A | input::DpadUp));

  cpu.gpr[3] = 0x1122334455667788ull;
  assert(!f.bridge.dispatch(0xDEADu, cpu, f.mem));
  assert(cpu.gpr[3] == 0x1122334455667788ull);
}

void test_null_and_fault_semantics_use_memory_v2() {
  Fixture f;
  // XamInputGetState permits a null output as a connectivity query.
  assert(f.bridge.get_state(f.mem, 0, xam::kFlagGamepad, 0) ==
         xam::result::Success);
  assert(f.bridge.get_capabilities(f.mem, 0, xam::kFlagGamepad, 0) ==
         xam::result::BadArguments);
  assert(f.bridge.set_state(f.mem, 0, 0, 0) == xam::result::BadArguments);

  bool faulted = false;
  try {
    static_cast<void>(
        f.bridge.get_state(f.mem, 0, xam::kFlagGamepad, 0x00200000u));
  } catch (const memory::MemoryFault& fault) {
    faulted = true;
    assert(fault.fault_address() == 0x00200000u);
    assert(fault.info().is_write());
  }
  assert(faulted);
}


void test_cpu_v1_external_registry_bridge() {
  Fixture f;
  xenon::cpu::ExternalCallRegistry registry;
  assert(guest::register_exports(registry, f.bridge));
  assert(registry.contains("xam", guest::ordinal::XamInputGetState));
  assert(registry.descriptors().size() == guest::exports().size());

  xenon::cpu::RegistryRuntimeServices runtime(registry);
  constexpr xenon::cpu::GuestAddress kState = 0x00100200u;
  xenon::cpu::CpuState cpu{};
  cpu.gpr[3] = 0;
  cpu.gpr[4] = xam::kFlagGamepad;
  cpu.gpr[5] = kState;
  assert(runtime.external_call("XAM", guest::ordinal::XamInputGetState, cpu,
                               f.mem));
  assert(cpu.gpr[3] == xam::result::Success);
  assert(f.mem.read16_be(kState + 4u) == (input::A | input::DpadUp));
  assert(!runtime.external_call("xboxkrnl", 0x191u, cpu, f.mem));
}

void test_export_catalog() {
  const auto table = guest::exports();
  assert(table.size() == 6);
  assert(table[0].ordinal == guest::ordinal::XamInputGetCapabilities);
  assert(table[0].name == "XamInputGetCapabilities");
  assert(table.back().ordinal == guest::ordinal::XamInputGetCapabilitiesEx);
}

}  // namespace

int main() {
  test_guest_layout_and_state_marshalling();
  test_capabilities_and_vibration_marshalling();
  test_keystroke_and_ex_marshalling();
  test_ppc_ordinal_dispatch();
  test_null_and_fault_semantics_use_memory_v2();
  test_export_catalog();
  test_cpu_v1_external_registry_bridge();
  return 0;
}
