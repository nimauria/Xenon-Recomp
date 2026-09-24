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
  result.insert(QStringLiteral("nativeExtensionPath"), native_extension_path);
  result.insert(QStringLiteral("adaptiveObservationPath"), adaptive_observation_path);
  result.insert(QStringLiteral("moduleSettings"), module_settings);
  result.insert(QStringLiteral("runtimeApiRequirements"), runtime_api_requirements);
  result.insert(QStringLiteral("profileId"), profile_id);
  result.insert(QStringLiteral("profileName"), profile_name);
  result.insert(QStringLiteral("region"), region);
  result.insert(QStringLiteral("renderer"), renderer);
  result.insert(QStringLiteral("shaderCache"), shader_cache);
  result.insert(QStringLiteral("shaderCacheMode"), shader_cache_mode);
  result.insert(QStringLiteral("inputBackend"), input_backend);
  result.insert(QStringLiteral("inputPreferredDevice"), input_preferred_device);
  result.insert(QStringLiteral("inputDeadzone"), input_deadzone);
  result.insert(QStringLiteral("inputRumble"), input_rumble);
  result.insert(QStringLiteral("inputBackground"), input_background);
  result.insert(QStringLiteral("inputModuleApiVersion"), input_module_api_version);
  result.insert(QStringLiteral("inputProfileStorePath"), input_profile_store_path);
  result.insert(QStringLiteral("inputUserSources"), input_user_sources);
  result.insert(QStringLiteral("audioMasterVolume"), audio_master_volume);
  result.insert(QStringLiteral("audioMuteUnfocused"), audio_mute_unfocused);
  result.insert(QStringLiteral("audioLatencyProfile"), audio_latency_profile);
  result.insert(QStringLiteral("logVerbose"), verbose_logging);
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
