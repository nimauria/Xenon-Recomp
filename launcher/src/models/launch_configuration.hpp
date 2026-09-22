#pragma once

#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher {

// Stable launcher-side contract assembled before control crosses RuntimeBridge.
// Keep game-specific values inside module_settings rather than adding per-title
// fields to this generic structure.
struct LaunchConfiguration {
  QString game_id;
  QString title;
  QString content_path;

  QString module_id;
  QString module_name;
  QString module_path;
  QString module_version;
  QVariantMap module_settings;
  QVariantMap runtime_api_requirements;
  // Absolute path to the module's compiled-code native extension library
  // (see docs/runtime/RUNTIME_HOST.md), or empty if the module declares none. The
  // runtime host cannot run guest code without this.
  QString native_extension_path;

  QString profile_id;
  QString profile_name;
  QString region;

  QString renderer;
  bool shader_cache = true;
  QString shader_cache_mode;
  QString input_backend;
  QString input_preferred_device;
  double input_deadzone = 0.10;
  bool input_rumble = true;
  bool input_background = false;
  int input_module_api_version = 1;
  QString input_profile_store_path;
  QVariantList input_user_sources;
  double audio_master_volume = 1.0;
  bool audio_mute_unfocused = false;
  QString audio_latency_profile;
  bool verbose_logging = false;
  QString game_root;
  QString managed_game_path;
  QVariantList dlc;
  QString save_path;
  QString screenshots_path;
  bool offline = true;
  bool isolated_settings = false;

  [[nodiscard]] QVariantMap toVariantMap() const;
};

}  // namespace xenon::launcher
