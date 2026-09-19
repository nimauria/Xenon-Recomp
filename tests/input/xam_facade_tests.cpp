#include <cassert>
#include <memory>

#include "xenon/input/keyboard_mouse_driver.hpp"
#include "xenon/input/xam_facade.hpp"

namespace input = xenon::input;
namespace xam = xenon::input::xam;

namespace {

struct Fixture {
  input::InputSystem system{};
  input::KeyboardMouseInputDriver* keyboard{};
  xam::InputFacade facade;

  Fixture() : facade(system) {
    auto driver = std::make_unique<input::KeyboardMouseInputDriver>();
    keyboard = driver.get();
    assert(system.add_driver(std::move(driver)));
    assert(system.setup() == input::Result::Success);
  }
};

void test_state_and_flag_semantics() {
  Fixture f;
  f.keyboard->set_key(static_cast<input::KeyCode>(input::Key::Space), true);
  input::State state{};
  assert(f.facade.get_state(0, xam::kFlagGamepad, &state) == xam::result::Success);
  assert((state.gamepad.buttons & input::A) != 0);
  assert(f.facade.get_state(xam::kUserIndexAny, xam::kFlagAnyUser, &state) ==
         xam::result::Success);
  assert(f.facade.get_state(0, 0x02, &state) == xam::result::DeviceNotConnected);
  assert(f.facade.get_state(0, 0, nullptr) == xam::result::Success);
}

void test_capabilities_and_vibration_arguments() {
  Fixture f;
  input::Capabilities caps{};
  assert(f.facade.get_capabilities(0, xam::kFlagGamepad, &caps) ==
         xam::result::Success);
  assert(f.facade.get_capabilities(0, 0, nullptr) == xam::result::BadArguments);
  assert(f.facade.get_capabilities_ex(123, 0, 0, &caps) == xam::result::Success);
  assert(f.facade.set_state(0, 0, nullptr) == xam::result::BadArguments);
  const input::Vibration vibration{1, 2};
  assert(f.facade.set_state(0, 0, &vibration) == xam::result::FunctionFailed);
}

void test_keystroke_and_ex_writeback() {
  Fixture f;
  f.keyboard->set_key(static_cast<input::KeyCode>(input::Key::Space), true);
  input::Keystroke key{};
  assert(f.facade.get_keystroke(0, xam::kFlagGamepad, &key) ==
         xam::result::Success);
  assert(key.virtual_key == 0x5800 && key.user_index == 0);
  assert(f.facade.get_keystroke(0, 0, nullptr) == xam::result::BadArguments);

  f.keyboard->set_key(static_cast<input::KeyCode>(input::Key::Space), false);
  std::uint32_t user = xam::kUserIndexAny;
  assert(f.facade.get_keystroke_ex(user, xam::kFlagAnyUser, &key) ==
         xam::result::Success);
  assert(user == 0);
  assert(key.flags == input::KeystrokeKeyUp);
}

void test_result_codes_match_xbox_values() {
  assert(xam::map_result(input::Result::Success) == 0);
  assert(xam::map_result(input::Result::BadArguments) == 0xA0);
  assert(xam::map_result(input::Result::DeviceNotConnected) == 0x48F);
  assert(xam::map_result(input::Result::Failed) == 0x65B);
  assert(xam::map_result(input::Result::Empty) == 0x10D2);
}

}  // namespace

int main() {
  test_state_and_flag_semantics();
  test_capabilities_and_vibration_arguments();
  test_keystroke_and_ex_writeback();
  test_result_codes_match_xbox_values();
  return 0;
}
