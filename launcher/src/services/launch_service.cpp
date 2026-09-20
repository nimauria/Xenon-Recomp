#include "launch_service.hpp"

#include "dlc_service.hpp"
#include "library_service.hpp"
#include "module_service.hpp"
#include "path_service.hpp"
#include "profile_service.hpp"
#include "settings_service.hpp"
#include "../runtime/runtime_bridge.hpp"

#include <QDir>

namespace xenon::launcher {

LaunchService::LaunchService(SettingsService& settings, PathService& paths,
                             ProfileService& profiles, LibraryService& library,
                             ModuleService& modules, DlcService& dlc,
                             FilesystemService& filesystem, IRuntimeBridge& runtime)
    : settings_(settings), paths_(paths), profiles_(profiles), library_(library),
      modules_(modules), dlc_(dlc), filesystem_(filesystem), runtime_(runtime) {}

std::optional<LaunchConfiguration> LaunchService::configurationFor(const QString& game_id) const {
  const auto game = library_.entry(game_id);
  if (game.isEmpty()) return std::nullopt;

  const auto profile = profiles_.activeProfile();
  const auto module_id = game.value(QStringLiteral("moduleId")).toString();
  const auto module = modules_.module(module_id);

  const auto game_path_override = profile.value(QStringLiteral("gamePath")).toString();
  const auto save_path_override = profile.value(QStringLiteral("savePath")).toString();
  const auto screenshots_override = profile.value(QStringLiteral("screenshotPath")).toString();

  LaunchConfiguration config;
  config.game_id = game.value(QStringLiteral("gameId")).toString();
  config.title = game.value(QStringLiteral("title")).toString();
  config.content_path = game.value(QStringLiteral("contentPath")).toString();
  config.module_id = module_id;
  config.module_name = game.value(QStringLiteral("moduleName")).toString();
  config.module_path = module.value(QStringLiteral("path")).toString();
  config.module_version = module.value(QStringLiteral("version")).toString();
  config.module_settings = modules_.settingsValues(module_id);
  config.runtime_api_requirements = modules_.runtimeApiRequirements(module_id);
  config.native_extension_path = modules_.nativeExtensionPath(module_id);
  config.profile_id = profile.value(QStringLiteral("profileId")).toString();
  config.profile_name = profile.value(QStringLiteral("profileName")).toString();
  config.region = profile.value(QStringLiteral("region"), QStringLiteral("Auto (Global)")).toString();

  const auto isolated = profile.value(QStringLiteral("isolatedSettings")).toBool();
  const auto overrides = isolated ? profile.value(QStringLiteral("runtimeOverrides")).toMap() : QVariantMap{};
  const auto overrideOr = [&](const QString& key, const QVariant& fallback) -> QVariant {
    return overrides.contains(key) ? overrides.value(key) : fallback;
  };

  config.renderer = overrideOr(QStringLiteral("runtime/graphicsBackend"),
                               settings_.stringValue(QStringLiteral("frontend/runtime/graphicsBackend"),
                                                     QStringLiteral("Automatic"))).toString();
  config.shader_cache = overrideOr(QStringLiteral("graphics/shaderCache"),
                                   settings_.boolValue(QStringLiteral("frontend/graphics/shaderCache"), true)).toBool();
  config.shader_cache_mode = overrideOr(QStringLiteral("graphics/shaderCacheMode"),
                                        settings_.stringValue(QStringLiteral("frontend/graphics/shaderCacheMode"),
                                                              QStringLiteral("Persistent"))).toString();
  config.input_backend = overrideOr(QStringLiteral("input/backend"),
                                    settings_.stringValue(QStringLiteral("frontend/input/backend"),
                                                          QStringLiteral("Automatic"))).toString();
  config.input_preferred_device = overrideOr(QStringLiteral("input/preferredDevice"),
                                             settings_.stringValue(QStringLiteral("frontend/input/preferredDevice"),
                                                                   QStringLiteral("Automatic"))).toString();
  config.input_deadzone = overrideOr(QStringLiteral("input/deadzone"),
                                     settings_.numberValue(QStringLiteral("frontend/input/deadzone"), 0.10)).toDouble();
  config.input_rumble = overrideOr(QStringLiteral("input/rumble"),
                                   settings_.boolValue(QStringLiteral("frontend/input/rumble"), true)).toBool();
  config.input_background = overrideOr(QStringLiteral("input/backgroundInput"),
                                       settings_.boolValue(QStringLiteral("frontend/input/backgroundInput"), false)).toBool();
  config.input_module_api_version =
      runtime_.capabilities().value(QStringLiteral("inputModuleApiVersion"), 0).toInt();
  config.input_profile_store_path = QDir{paths_.configuredPath(QStringLiteral("profiles"))}
      .filePath(QStringLiteral("input-profiles-v1.conf"));
  for (int user = 0; user < 4; ++user) {
    const auto sources = settings_.value(QStringLiteral("frontend/input/user%1/sources").arg(user)).toStringList();
    config.input_user_sources.append(QVariantMap{{QStringLiteral("userIndex"), user},
                                                 {QStringLiteral("sources"), sources}});
  }
  config.audio_master_volume = overrideOr(QStringLiteral("audio/masterVolume"),
                                          settings_.numberValue(QStringLiteral("frontend/audio/masterVolume"), 1.0)).toDouble();
  config.audio_mute_unfocused = overrideOr(QStringLiteral("audio/muteUnfocused"),
                                           settings_.boolValue(QStringLiteral("frontend/audio/muteUnfocused"), false)).toBool();
  config.audio_latency_profile = overrideOr(QStringLiteral("audio/latencyProfile"),
                                            settings_.stringValue(QStringLiteral("frontend/audio/latencyProfile"),
                                                                  QStringLiteral("Automatic"))).toString();
  config.game_root = game_path_override.isEmpty() ? paths_.configuredPath(QStringLiteral("games"))
                                                  : game_path_override;
  config.managed_game_path = library_.managedPath(game_id);
  config.dlc = dlc_.launchEntries(game_id);
  const auto save_root = save_path_override.isEmpty() ? paths_.configuredPath(QStringLiteral("saves"))
                                                       : save_path_override;
  const auto screenshots_root = screenshots_override.isEmpty()
      ? paths_.configuredPath(QStringLiteral("screenshots"))
      : screenshots_override;
  config.save_path = QDir{save_root}.filePath(game_id);
  config.screenshots_path = QDir{screenshots_root}.filePath(game_id);
  config.offline = profile.contains(QStringLiteral("offline"))
      ? profile.value(QStringLiteral("offline")).toBool()
      : settings_.boolValue(QStringLiteral("frontend/runtime/offline"), true);
  config.isolated_settings = isolated;
  return config;
}

ServiceResult LaunchService::validate(const QString& game_id) const {
  const auto game = library_.entry(game_id);
  if (game.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Launch failed"),
                                  QStringLiteral("The selected game is not in the launcher library."));
  }
  if (!game.value(QStringLiteral("ready")).toBool()) {
    return ServiceResult::failure(QStringLiteral("Game not ready"),
                                  QStringLiteral("This library entry has not yet been identified and validated by a compatible game module."));
  }

  const auto module_id = game.value(QStringLiteral("moduleId")).toString();
  if (module_id.trimmed().isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module required"),
                                  QStringLiteral("The selected game does not have a Xenon game module assigned."));
  }

  const auto module = modules_.module(module_id);
  if (module.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module missing"),
                                  QStringLiteral("The Xenon module assigned to this game is not installed."));
  }
  if (!module.value(QStringLiteral("active")).toBool()) {
    return ServiceResult::failure(QStringLiteral("Module disabled"),
                                  QStringLiteral("Enable the assigned Xenon module before launching this game."));
  }

  const auto module_verification = modules_.verify(module_id);
  if (!module_verification.ok) return module_verification;

  const auto config = configurationFor(game_id);
  if (!config.has_value()) {
    return ServiceResult::failure(QStringLiteral("Launch failed"),
                                  QStringLiteral("Xenon could not assemble a launch configuration for this game."));
  }
  return runtime_.prepareLaunch(*config);
}

ServiceResult LaunchService::startValidated(const QString& game_id) {
  const auto config = configurationFor(game_id);
  if (!config.has_value()) {
    return ServiceResult::failure(QStringLiteral("Launch failed"),
                                  QStringLiteral("Xenon could not assemble a launch configuration for this game."));
  }

  const auto result = runtime_.launch(*config);
  if (result.ok) (void)library_.markLaunched(game_id);
  return result;
}

ServiceResult LaunchService::launch(const QString& game_id) {
  const auto validation = validate(game_id);
  if (!validation.ok) return validation;
  return startValidated(game_id);
}

ServiceResult LaunchService::stop() { return runtime_.stop(); }

}  // namespace xenon::launcher
