#pragma once

#ifndef XENON_LAUNCHER_TEST_MODE
#define XENON_LAUNCHER_TEST_MODE 0
#endif

namespace xenon::launcher {

// Optional source-level switch for very quick UI iteration. Keep this false in
// committed production builds; developers can either flip it locally or use
// -DXENON_LAUNCHER_TEST_MODE=ON at configure time.
inline constexpr bool kForceTestMode = false;
inline constexpr bool kTestMode = kForceTestMode || (XENON_LAUNCHER_TEST_MODE != 0);

// Front-end capability contract. The UI asks this table what should exist
// rather than assuming every runtime service is always present. These compile-
// time flags are the first seam; later the runtime service registry can layer
// live capabilities on top without redesigning the QML pages.
struct UiFeatures {
  bool settings_general = true;
  bool settings_system = true;
  bool settings_appearance = true;
  bool settings_library = true;
  bool settings_paths = true;
  bool settings_runtime = true;
  bool settings_graphics = true;
  bool settings_input = true;
  bool settings_audio = true;
  bool settings_network = false;
  bool settings_filesystem = true;
  bool settings_updates = true;
  bool settings_community = true;
  bool settings_accessibility = true;
  bool settings_developer = true;
  bool settings_about = true;

  bool library_dlc = true;
  bool library_manage_files = true;
  bool modules_updates = true;
  bool modules_catalog = true;
  bool modules_settings = true;
  bool profiles_paths = true;
  bool profiles_avatars = true;
};

inline constexpr UiFeatures kUiFeatures{};

}  // namespace xenon::launcher
