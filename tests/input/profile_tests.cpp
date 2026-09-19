#include <cassert>
#include <chrono>
#include <filesystem>
#include <memory>

#include "xenon/input/keyboard_mouse_driver.hpp"
#include "xenon/input/profile.hpp"
#include "xenon/input/system.hpp"

namespace input = xenon::input;

namespace {

class TempFile {
 public:
  TempFile() {
    path_ = std::filesystem::temp_directory_path() /
            ("xenon_input_profiles_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".cfg");
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }
 private:
  std::filesystem::path path_{};
};

input::InputProfile inverted_profile(std::string id) {
  input::InputProfile profile{};
  profile.id = std::move(id);
  profile.name = "Inverted + deadzone";
  profile.left_stick.invert_x = true;
  profile.left_stick.inner_deadzone = 0.20f;
  profile.left_stick.response_exponent = 2.0f;
  profile.right_stick.invert_y = true;
  profile.left_trigger.inner_deadzone = 0.10f;
  return profile;
}

void test_profile_validation_and_transform() {
  auto profile = inverted_profile("flight");
  std::string error;
  assert(input::validate_profile(profile, &error));
  input::GamepadState raw{};
  raw.thumb_lx = 32767;
  raw.thumb_ly = 1000;  // radial deadzone still preserves direction with large X.
  raw.thumb_ry = 16000;
  raw.left_trigger = 10;
  auto transformed = input::apply_profile(profile, raw);
  assert(transformed.thumb_lx < 0);
  assert(transformed.thumb_ry < 0);
  assert(transformed.left_trigger == 0);

  profile.left_stick.inner_deadzone = 0.8f;
  profile.left_stick.outer_deadzone = 0.3f;
  assert(!input::validate_profile(profile, &error));
}

void test_store_precedence_and_diagnostics() {
  input::ProfileStore store;
  auto device_profile = inverted_profile("device");
  auto user_profile = inverted_profile("user");
  user_profile.left_stick.invert_x = false;
  assert(store.upsert(device_profile));
  assert(store.upsert(user_profile));
  assert(store.bind_device("sdl:pad-123", "device"));
  assert(store.resolve(0, "sdl:pad-123")->id == "device");
  assert(store.bind_user(0, "user"));
  assert(store.resolve(0, "sdl:pad-123")->id == "user");
  const auto diagnostics = store.diagnostics();
  assert(diagnostics.profile_count == 3);
  assert(diagnostics.user_binding_count == 1);
  assert(diagnostics.device_binding_count == 1);
}

void test_persistence_roundtrip() {
  input::ProfileStore store;
  auto profile = inverted_profile("persistent");
  profile.left_stick.sensitivity = 0.75f;
  profile.right_trigger.invert = true;
  assert(store.upsert(profile));
  assert(store.set_default("persistent"));
  assert(store.bind_user(2, "persistent"));
  assert(store.bind_device("keyboard-mouse:keyboard-mouse:default", "persistent"));

  const auto serialized = store.serialize();
  assert(serialized == store.serialize());  // stable ordering for clean diffs.
  input::ProfileStore copy;
  assert(copy.deserialize(serialized));
  assert(copy.resolve(2, "anything")->id == "persistent");
  assert(copy.resolve(0, "keyboard-mouse:keyboard-mouse:default")->id == "persistent");
  assert(copy.profile("persistent")->right_trigger.invert);

  TempFile file;
  assert(copy.save(file.path()));
  input::ProfileStore loaded;
  assert(loaded.load(file.path()));
  assert(loaded.diagnostics().default_profile_id == "persistent");
}

void test_profiles_apply_before_packet_comparison() {
  input::InputSystem system;
  auto driver = std::make_unique<input::KeyboardMouseInputDriver>();
  auto* keyboard = driver.get();
  assert(system.add_driver(std::move(driver)));
  assert(system.setup() == input::Result::Success);
  auto profile = inverted_profile("user-flight");
  profile.left_stick.inner_deadzone = 0.0f;
  profile.left_stick.response_exponent = 1.0f;
  assert(system.profiles().upsert(profile));
  assert(system.profiles().bind_user(0, "user-flight"));

  keyboard->set_key(static_cast<input::KeyCode>(input::Key::D), true);
  input::State transformed{};
  assert(system.get_state(0, transformed) == input::Result::Success);
  assert(transformed.gamepad.thumb_lx < -30000);
  const auto packet = transformed.packet_number;
  assert(system.get_state(0, transformed) == input::Result::Success);
  assert(transformed.packet_number == packet);

  auto normal = profile;
  normal.id = "normal";
  normal.left_stick.invert_x = false;
  assert(system.profiles().upsert(normal));
  assert(system.profiles().bind_user(0, "normal"));
  assert(system.get_state(0, transformed) == input::Result::Success);
  assert(transformed.gamepad.thumb_lx > 30000);
  assert(transformed.packet_number == packet + 1);
}

}  // namespace

int main() {
  test_profile_validation_and_transform();
  test_store_precedence_and_diagnostics();
  test_persistence_roundtrip();
  test_profiles_apply_before_packet_comparison();
  return 0;
}
