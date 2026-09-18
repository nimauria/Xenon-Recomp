#include "launch_configuration.hpp"

namespace xenon::launcher {

QVariantMap LaunchConfiguration::toVariantMap() const {
  QVariantMap result;
  result.insert(QStringLiteral("gameId"), game_id);
  result.insert(QStringLiteral("title"), title);
  result.insert(QStringLiteral("contentPath"), content_path);
  result.insert(QStringLiteral("moduleId"), module_id);
  result.insert(QStringLiteral("moduleName"), module_name);
  result.insert(QStringLiteral("modulePath"), module_path);
  result.insert(QStringLiteral("moduleVersion"), module_version);
  result.insert(QStringLiteral("moduleSettings"), module_settings);
  result.insert(QStringLiteral("profileId"), profile_id);
  result.insert(QStringLiteral("profileName"), profile_name);
  result.insert(QStringLiteral("region"), region);
  result.insert(QStringLiteral("renderer"), renderer);
  result.insert(QStringLiteral("shaderCache"), shader_cache);
  result.insert(QStringLiteral("shaderCacheMode"), shader_cache_mode);
  result.insert(QStringLiteral("inputPreferredDevice"), input_preferred_device);
  result.insert(QStringLiteral("inputDeadzone"), input_deadzone);
  result.insert(QStringLiteral("inputRumble"), input_rumble);
  result.insert(QStringLiteral("audioMasterVolume"), audio_master_volume);
  result.insert(QStringLiteral("audioMuteUnfocused"), audio_mute_unfocused);
  result.insert(QStringLiteral("audioLatencyProfile"), audio_latency_profile);
  result.insert(QStringLiteral("gameRoot"), game_root);
  result.insert(QStringLiteral("managedGamePath"), managed_game_path);
  result.insert(QStringLiteral("dlc"), dlc);
  result.insert(QStringLiteral("savePath"), save_path);
  result.insert(QStringLiteral("screenshotsPath"), screenshots_path);
  result.insert(QStringLiteral("offline"), offline);
  result.insert(QStringLiteral("isolatedSettings"), isolated_settings);
  return result;
}

}  // namespace xenon::launcher
