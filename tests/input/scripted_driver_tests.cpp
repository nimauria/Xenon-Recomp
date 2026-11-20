#include <cassert>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "xenon/input/scripted_driver.hpp"
#include "xenon/input/system.hpp"

namespace input = xenon::input;
using std::chrono::milliseconds;

namespace {

input::ScriptedInput parse(const char* text) {
  std::string error;
  auto script = input::parse_input_script(text, error);
  assert(script && error.empty());
  return *script;
}

bool rejects(const char* text, const char* fragment) {
  std::string error;
  const auto script = input::parse_input_script(text, error);
  return !script && error.find(fragment) != std::string::npos;
}

void test_parses_times_actions_and_repeats() {
  const auto script = parse(
      "# boot\n"
      "2s press A\n"
      "+1500ms press START+B for 300ms   # comment\n"
      "\n"
      "+0.5s stick left -1 0.5 for 2s\n"
      "10s trigger right 1 for 1s\n"
      "11s repeat 3 every 1s press DOWN for 50ms\n"
      "20s log reached the hangar\n");
  assert(script.events.size() == 8);
  assert(script.events[0].start == milliseconds(2000));
  assert(script.events[0].duration == milliseconds(100));
  assert(script.events[0].buttons == input::A);
  assert(script.events[1].start == milliseconds(3500));
  assert(script.events[1].duration == milliseconds(300));
  assert(script.events[1].buttons == (input::Start | input::B));
  assert(script.events[2].start == milliseconds(4000));
  assert(script.events[2].kind == input::ScriptedInputEvent::Kind::LeftStick);
  assert(script.events[2].x == -32768 && script.events[2].y == 16384);
  assert(script.events[3].trigger == 255);
  assert(script.events[4].start == milliseconds(11000));
  assert(script.events[5].start == milliseconds(12000));
  assert(script.events[6].start == milliseconds(13000));
  assert(script.events[6].duration == milliseconds(50));
  assert(script.events[7].kind == input::ScriptedInputEvent::Kind::Log);
  assert(script.events[7].text == "reached the hangar");
  assert(script.end() == milliseconds(20000));
}

void test_events_are_ordered_by_start() {
  // Absolute times may go back; relative ones follow the previous line.
  const auto script = parse("5s press A\n1s press B\n+1s press X\n");
  assert(script.events[0].buttons == input::B && script.events[0].start == milliseconds(1000));
  assert(script.events[1].buttons == input::X && script.events[1].start == milliseconds(2000));
  assert(script.events[2].buttons == input::A);
}

void test_rejects_malformed_lines_with_their_number() {
  assert(rejects("2 press A\n", "line 1: bad start time"));
  assert(rejects("1s\n", "line 1: missing action"));
  assert(rejects("1s press A\n2s press Q\n", "line 2: unknown button 'Q'"));
  assert(rejects("1s press A for 0ms\n", "bad duration"));
  assert(rejects("1s press A for soon\n", "bad duration"));
  assert(rejects("1s stick up 0 0\n", "left or right"));
  assert(rejects("1s stick left 2 0\n", "[-1, 1]"));
  assert(rejects("1s trigger left 1.5\n", "[0, 1]"));
  assert(rejects("1s repeat 0 every 1s press A\n", "whole number"));
  assert(rejects("1s repeat 2.5 every 1s press A\n", "whole number"));
  assert(rejects("1s repeat 2 every 0ms press A\n", "interval"));
  assert(rejects("1s repeat 2 press A\n", "repeat takes"));
  assert(rejects("1s jump\n", "unknown action 'jump'"));
  assert(rejects("-1s press A\n", "bad start time"));
}

void test_state_combines_buttons_and_latest_analog() {
  const auto script = parse(
      "0s press A for 1s\n"
      "500ms press B for 1s\n"
      "0s stick right 1 0 for 2s\n"
      "1s stick right 0 -1 for 500ms\n"
      "0s trigger left 0.5 for 3s\n");
  auto s = input::scripted_state_at(script, milliseconds(0));
  assert(s.buttons == input::A && s.thumb_rx == 32767 && s.left_trigger == 128);
  s = input::scripted_state_at(script, milliseconds(700));
  assert(s.buttons == (input::A | input::B));
  // The later stick event wins while both are active, then the earlier one returns.
  s = input::scripted_state_at(script, milliseconds(1200));
  assert(s.buttons == input::B && s.thumb_rx == 0 && s.thumb_ry == -32768);
  s = input::scripted_state_at(script, milliseconds(1500));
  assert(s.buttons == 0 && s.thumb_rx == 32767 && s.thumb_ry == 0);
  // An event ends exactly at start + duration.
  s = input::scripted_state_at(script, milliseconds(3000));
  assert(s == input::GamepadState{});
}

void test_driver_clock_starts_at_first_poll_and_reports_events() {
  auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
  std::vector<std::string> reported;
  input::ScriptedInputDriver driver(
      parse("1s press A\n2s log menu\n3s press B for 1s\n"), [&] { return now; },
      [&](const input::ScriptedInputEvent& e, milliseconds at) {
        reported.push_back(e.text + "@" + std::to_string(at.count()));
      });
  std::vector<input::DriverDeviceInfo> devices;
  driver.enumerate_devices(devices);
  assert(devices.size() == 1);
  assert(devices[0].persistent_key == "script");
  assert(devices[0].connection == input::ConnectionType::Virtual);
  assert(!driver.elapsed());

  // Time before the first poll does not count.
  now += std::chrono::seconds(30);
  input::GamepadState state{};
  assert(driver.get_state(devices[0].native_id, state) == input::Result::Success);
  assert(state == input::GamepadState{});
  assert(driver.elapsed() == milliseconds(0));

  now += milliseconds(1050);
  assert(driver.get_state(devices[0].native_id, state) == input::Result::Success);
  assert(state.buttons == input::A);
  assert(reported.size() == 1 && reported[0] == "1s press A@1050");

  // A poll that skips past several events reports each once.
  now += milliseconds(2500);
  assert(driver.get_state(devices[0].native_id, state) == input::Result::Success);
  assert(state.buttons == input::B);
  assert(reported.size() == 3);
  assert(reported[1] == "menu@3550" && reported[2] == "3s press B for 1s@3550");
  assert(driver.events_reached() == 3);

  assert(driver.get_state(99, state) == input::Result::DeviceNotConnected);
  input::Capabilities caps{};
  assert(driver.get_capabilities(devices[0].native_id, caps) == input::Result::Success);
  assert(caps.gamepad.right_trigger == 0xFF);
  input::Keystroke key{};
  assert(driver.get_keystroke(devices[0].native_id, key) == input::Result::Empty);
}

void test_scripted_controller_drives_a_user_through_the_input_system() {
  auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
  input::InputSystem system;
  assert(system.add_driver(std::make_unique<input::ScriptedInputDriver>(
      parse("0s press START for 1s\n"), [&] { return now; })));
  assert(system.setup() == input::Result::Success);
  const auto device = system.device_for_user(0);
  assert(device);
  // A scripted run plays with the window in the background.
  system.set_background_input_policy(input::BackgroundInputPolicy::Always);
  system.set_focused(false);
  input::State state{};
  assert(system.get_state(0, state) == input::Result::Success);
  assert(state.gamepad.buttons == input::Start);
  now += std::chrono::seconds(2);
  assert(system.get_state(0, state) == input::Result::Success);
  assert(state.gamepad.buttons == 0);
  system.shutdown();
}

}  // namespace

int main() {
  test_parses_times_actions_and_repeats();
  test_events_are_ordered_by_start();
  test_rejects_malformed_lines_with_their_number();
  test_state_combines_buttons_and_latest_analog();
  test_driver_clock_starts_at_first_poll_and_reports_events();
  test_scripted_controller_drives_a_user_through_the_input_system();
  return 0;
}
