// Unit tests for xenon::runtime_host::LaunchConfig::load_from_file - the
// input side of the launcher/runtime-host IPC contract documented in
// docs/runtime/RUNTIME_HOST.md. RuntimeBridge (launcher side) is exercised
// separately in launcher/tests/runtime/runtime_bridge_tests.cpp; this file
// only covers config parsing/validation in isolation, without any Qt or
// process-spawning dependency.

#include "launch_config.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using xenon::runtime_host::LaunchConfig;

class TempFile {
 public:
  explicit TempFile(std::string_view name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("xenon_launch_config_test_" + std::to_string(stamp) + "_" + std::string(name));
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  void write(std::string_view contents) const {
    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    file << contents;
  }

  [[nodiscard]] std::string string() const { return path_.string(); }

 private:
  std::filesystem::path path_{};
};

}  // namespace

int main() {
  std::cout << "Testing Xenon Runtime Host LaunchConfig...\n";

  // Test 1: a complete, valid config round-trips every field.
  {
    TempFile file("valid.json");
    file.write(R"json({
      "configVersion": 1,
      "sessionId": "session-123",
      "sessionDir": "C:/sessions/session-123",
      "gameId": "halo3",
      "title": "Halo 3",
      "contentPath": "C:/Games/Halo3",
      "moduleId": "org.example.halo3-module",
      "moduleName": "Halo 3 Module",
      "modulePath": "C:/Xenon/modules/halo3-module",
      "moduleVersion": "1.2.0",
      "nativeExtensionPath": "C:/Xenon/modules/halo3-module/native/halo3.dll",
      "profileId": "profile-1",
      "profileName": "Player One",
      "region": "Auto (Global)",
      "profileXuid": "16140901064495857665",
      "renderer": "Vulkan",
      "inputBackend": "XInput",
      "audioMasterVolume": 0.75,
      "audioMuteUnfocused": true,
      "audioLatencyProfile": "Low",
      "logVerbose": true,
      "titleUpdatePath": "C:/Games/Halo3/TU",
      "dlcRootPath": "C:/Games/Halo3/DLC",
      "dlc": [
        {"type": "DLC", "path": "C:/Games/Halo3/DLC/map1"},
        {"type": "TitleUpdate", "path": "C:/Games/Halo3/TU/tu1.xex"}
      ],
      "savePath": "C:/Xenon/saves/halo3",
      "screenshotsPath": "C:/Xenon/screenshots/halo3",
      "offline": false
    })json");

    LaunchConfig config{};
    std::string error;
    const bool ok = LaunchConfig::load_from_file(file.string(), config, error);
    assert(ok && "A well-formed config should parse successfully");
    assert(error.empty());
    assert(config.config_version == 1);
    assert(config.session_id == "session-123");
    assert(config.session_dir == "C:/sessions/session-123");
    assert(config.game_id == "halo3");
    assert(config.title == "Halo 3");
    assert(config.content_path == "C:/Games/Halo3");
    assert(config.module_id == "org.example.halo3-module");
    assert(config.native_extension_path ==
           "C:/Xenon/modules/halo3-module/native/halo3.dll");
    assert(config.profile_xuid == 16140901064495857665ULL);
    assert(config.renderer == "Vulkan");
    assert(config.input_backend == "XInput");
    assert(config.audio_master_volume == 0.75);
    assert(config.audio_mute_unfocused == true);
    assert(config.audio_latency_profile == "Low");
    assert(config.log_verbose == true);
    assert(config.title_update_path == "C:/Games/Halo3/TU");
    assert(config.dlc_root_path == "C:/Games/Halo3/DLC");
    assert(config.dlc.size() == 2);
    assert(config.dlc[0].type == "DLC");
    assert(config.dlc[0].path == "C:/Games/Halo3/DLC/map1");
    assert(config.dlc[1].type == "TitleUpdate");
    assert(config.save_path == "C:/Xenon/saves/halo3");
    assert(config.screenshots_path == "C:/Xenon/screenshots/halo3");
    assert(config.offline == false);
  }
  std::cout << "  [PASS] Valid config round-trips every field\n";

  // Test 2: a missing file fails cleanly with a descriptive error.
  {
    LaunchConfig config{};
    std::string error;
    const bool ok = LaunchConfig::load_from_file(
        (std::filesystem::temp_directory_path() / "xenon_does_not_exist.json").string(),
        config, error);
    assert(!ok && "A missing file must not parse successfully");
    assert(!error.empty());
  }
  std::cout << "  [PASS] Missing file is rejected\n";

  // Test 3: malformed JSON fails cleanly.
  {
    TempFile file("malformed.json");
    file.write("{ this is not valid json");

    LaunchConfig config{};
    std::string error;
    const bool ok = LaunchConfig::load_from_file(file.string(), config, error);
    assert(!ok && "Malformed JSON must not parse successfully");
    assert(!error.empty());
  }
  std::cout << "  [PASS] Malformed JSON is rejected\n";

  // Test 4: a JSON value that is not an object (e.g. a bare array) fails.
  {
    TempFile file("not_object.json");
    file.write("[1, 2, 3]");

    LaunchConfig config{};
    std::string error;
    const bool ok = LaunchConfig::load_from_file(file.string(), config, error);
    assert(!ok && "A non-object top-level JSON value must not parse successfully");
    assert(!error.empty());
  }
  std::cout << "  [PASS] Non-object top-level JSON is rejected\n";

  // Test 5: missing the required "sessionDir" field fails.
  {
    TempFile file("missing_session_dir.json");
    file.write(R"json({"contentPath": "C:/Games/Halo3"})json");

    LaunchConfig config{};
    std::string error;
    const bool ok = LaunchConfig::load_from_file(file.string(), config, error);
    assert(!ok && "A config missing sessionDir must be rejected");
    assert(error.find("sessionDir") != std::string::npos);
  }
  std::cout << "  [PASS] Missing sessionDir is rejected with a specific error\n";

  // Test 6: missing the required "contentPath" field fails.
  {
    TempFile file("missing_content_path.json");
    file.write(R"json({"sessionDir": "C:/sessions/x"})json");

    LaunchConfig config{};
    std::string error;
    const bool ok = LaunchConfig::load_from_file(file.string(), config, error);
    assert(!ok && "A config missing contentPath must be rejected");
    assert(error.find("contentPath") != std::string::npos);
  }
  std::cout << "  [PASS] Missing contentPath is rejected with a specific error\n";

  // Test 7: an absent configVersion defaults to 1 (backward compatibility
  // with a launcher build that predates versioning) rather than being
  // rejected.
  {
    TempFile file("no_version.json");
    file.write(R"json({"sessionDir": "C:/sessions/x", "contentPath": "C:/Games/X"})json");

    LaunchConfig config{};
    std::string error;
    const bool ok = LaunchConfig::load_from_file(file.string(), config, error);
    assert(ok && "A config with no configVersion should default to version 1");
    assert(config.config_version == 1);
  }
  std::cout << "  [PASS] Missing configVersion defaults to version 1\n";

  // Test 8: a configVersion newer than this build supports is rejected
  // up front, before any other field is trusted.
  {
    TempFile file("future_version.json");
    file.write(R"json({
      "configVersion": 999,
      "sessionDir": "C:/sessions/x",
      "contentPath": "C:/Games/X"
    })json");

    LaunchConfig config{};
    std::string error;
    const bool ok = LaunchConfig::load_from_file(file.string(), config, error);
    assert(!ok && "A configVersion newer than kCurrentVersion must be rejected");
    assert(error.find("999") != std::string::npos);
  }
  std::cout << "  [PASS] A too-new configVersion is rejected\n";

  // Test 9: a configVersion of 0 (or negative) is rejected as invalid,
  // rather than silently accepted.
  {
    TempFile file("zero_version.json");
    file.write(R"json({
      "configVersion": 0,
      "sessionDir": "C:/sessions/x",
      "contentPath": "C:/Games/X"
    })json");

    LaunchConfig config{};
    std::string error;
    const bool ok = LaunchConfig::load_from_file(file.string(), config, error);
    assert(!ok && "A configVersion of 0 must be rejected");
  }
  std::cout << "  [PASS] A zero configVersion is rejected\n";

  // Test 10: renderer/inputBackend default to "Automatic" when omitted, and
  // offline defaults to true.
  {
    TempFile file("defaults.json");
    file.write(R"json({"sessionDir": "C:/sessions/x", "contentPath": "C:/Games/X"})json");

    LaunchConfig config{};
    std::string error;
    const bool ok = LaunchConfig::load_from_file(file.string(), config, error);
    assert(ok);
    assert(config.renderer == "Automatic");
    assert(config.input_backend == "Automatic");
    assert(config.audio_master_volume == 1.0);
    assert(config.audio_mute_unfocused == false);
    assert(config.audio_latency_profile.empty());
    assert(config.log_verbose == false);
    assert(config.offline == true);
    assert(config.dlc.empty());
  }
  std::cout << "  [PASS] Optional fields fall back to documented defaults\n";

  // Test 11 (Part 5.1 of the Gracemeria readiness pass): headlessMode
  // defaults to false (every ordinary launcher Play action is Normal Play -
  // see main.cpp's strict content/presentation/audio failure semantics,
  // which only relax when this is explicitly true).
  {
    TempFile file("no_headless.json");
    file.write(R"json({"sessionDir": "C:/sessions/x", "contentPath": "C:/Games/X"})json");

    LaunchConfig config{};
    std::string error;
    const bool ok = LaunchConfig::load_from_file(file.string(), config, error);
    assert(ok);
    assert(config.headless_mode == false &&
           "headlessMode must default to false - an ordinary launch is always Normal Play");
  }
  std::cout << "  [PASS] headlessMode defaults to false (Normal Play)\n";

  // Test 12: an explicit headlessMode=true round-trips.
  {
    TempFile file("headless.json");
    file.write(R"json({
      "sessionDir": "C:/sessions/x",
      "contentPath": "C:/Games/X",
      "headlessMode": true
    })json");

    LaunchConfig config{};
    std::string error;
    const bool ok = LaunchConfig::load_from_file(file.string(), config, error);
    assert(ok);
    assert(config.headless_mode == true);
  }
  std::cout << "  [PASS] Explicit headlessMode=true round-trips\n";

  std::cout << "All LaunchConfig tests passed!\n";
  return 0;
}
