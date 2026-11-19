#pragma once

// The game-specific half of keyboard and mouse play: which keys drive which
// Xbox controls in each part of a title (menus, flying, on foot...). Xenon
// owns the virtual controller (keyboard_mouse_driver.hpp); a module supplies
// the bindings per revision in its hint package, read at launch:
//
//   <module>/xenon-analysis/revisions/<effective image hash>/input.json
//   {
//     "schema": "xenon.input.v1",
//     "keyboard": {
//       "initialContext": "menu",
//       "contexts": [
//         {
//           "name": "flight",
//           "keys": [["W", "RightTrigger"], ["A", "LeftStickLeft"],
//                    ["2", "LeftTrigger"], ["2", "RightTrigger"],
//                    ["Q", "LeftStickLeft", 0.5]],
//           "mouseButtons": [["Left", "ButtonA"]],
//           "mouse": { "stick": "left", "sensitivityX": 1.5, "sensitivityY": 1.5,
//                      "invertX": false, "invertY": true, "smoothing": 0.035,
//                      "antiDeadzone": 0.15, "alternateStickKey": "LeftAlt" }
//         }
//       ]
//     }
//   }
//
// A binding is [key, control] or [key, control, scale]; one key may drive
// several controls. Key, control and mouse-button names are those of
// key_from_name() / virtual_control_from_name() / mouse_button_from_name().
// "mouse" is optional; "stick" is "left", "right" or "none". The module
// selects a context at run time through cpu::ModuleServicesV1.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/input/keyboard_mouse_driver.hpp"

namespace xenon::input {

inline constexpr std::string_view kModuleInputProfileSchema = "xenon.input.v1";
inline constexpr std::string_view kModuleInputProfileFile = "input.json";

struct ModuleInputProfile {
  std::vector<KeyboardMouseContext> keyboard_contexts{};
  std::string initial_context{};  // empty: the default bindings
};

// Parses input.json. Any unknown name, malformed binding or out-of-range value
// is an error naming it (a profile that half-loads would leave controls
// silently unbound).
[[nodiscard]] std::optional<ModuleInputProfile> parse_module_input_profile(std::string_view json,
                                                                           std::string* error);

struct ModuleInputProfileLoad {
  std::optional<ModuleInputProfile> profile{};
  std::filesystem::path path{};  // the file read, empty when there was none
  std::string error{};           // non-empty: the file exists and is invalid
};

[[nodiscard]] ModuleInputProfileLoad load_module_input_profile(
    const std::filesystem::path& package, std::string_view effective_hash_hex);

// The default options with the profile's contexts added.
void apply_module_input_profile(const ModuleInputProfile& profile, KeyboardMouseOptions& options);

}  // namespace xenon::input
