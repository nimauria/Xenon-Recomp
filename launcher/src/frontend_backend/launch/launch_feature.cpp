#include "launch_feature.hpp"

#include "../library/dlc/dlc_feature.hpp"
#include "../library/library_feature.hpp"
#include "../modules/modules_feature.hpp"
#include "../paths/paths_feature.hpp"
#include "../profiles/profiles_feature.hpp"
#include "../settings/settings_feature.hpp"

#include <QDir>

namespace xenon::launcher::frontend_backend {

LaunchFeature::LaunchFeature(LaunchService& launch, LibraryFeature& library, DlcFeature& dlc,
                             ModulesFeature& modules, ProfilesFeature& profiles,
                             SettingsFeature& settings, PathsFeature& paths, bool test_mode)
    : launch_(launch), library_(library), dlc_(dlc), modules_(modules), profiles_(profiles),
      settings_(settings), paths_(paths), test_mode_(test_mode) {}

void LaunchFeature::ensureModuleResolved(const QString& game_id) const {
  if (!test_mode_) launch_.ensureModuleResolved(game_id);
}

QVariantMap LaunchFeature::configurationFor(const QString& game_id) const {
  if (!test_mode_) {
    const auto config = launch_.configurationFor(game_id);
    return config.has_value() ? config->toVariantMap() : QVariantMap{};
  }

  const auto game = library_.entry(game_id);
  if (game.isEmpty()) return {};
  const auto profile = profiles_.activeProfile();
  const auto module_id = game.value(QStringLiteral("moduleId")).toString();
  const auto module = modules_.module(module_id);

  QVariantMap config;
  config.insert(QStringLiteral("gameId"), game_id);
  config.insert(QStringLiteral("title"), game.value(QStringLiteral("title")));
  config.insert(QStringLiteral("contentPath"), game.value(QStringLiteral("contentPath")));
  config.insert(QStringLiteral("moduleId"), module_id);
  config.insert(QStringLiteral("moduleName"), game.value(QStringLiteral("moduleName")));
  config.insert(QStringLiteral("modulePath"), module.value(QStringLiteral("path")));
  config.insert(QStringLiteral("moduleVersion"), module.value(QStringLiteral("version"), game.value(QStringLiteral("moduleVersion"))));
  config.insert(QStringLiteral("moduleSettings"), modules_.settingsValues(module_id));
  config.insert(QStringLiteral("profileId"), profile.value(QStringLiteral("profileId")));
  config.insert(QStringLiteral("profileName"), profile.value(QStringLiteral("profileName")));
  config.insert(QStringLiteral("region"), profile.value(QStringLiteral("region"), QStringLiteral("Auto (Global)")));
  const auto runtime_settings = profiles_.resolvedRuntimeSettings(profile);
  config.insert(QStringLiteral("renderer"), runtime_settings.value(QStringLiteral("runtime/graphicsBackend"), QStringLiteral("Automatic")));
  config.insert(QStringLiteral("shaderCache"), runtime_settings.value(QStringLiteral("graphics/shaderCache"), true));
  config.insert(QStringLiteral("shaderCacheMode"), runtime_settings.value(QStringLiteral("graphics/shaderCacheMode"), QStringLiteral("Persistent")));
  config.insert(QStringLiteral("inputPreferredDevice"), runtime_settings.value(QStringLiteral("input/preferredDevice"), QStringLiteral("Automatic")));
  config.insert(QStringLiteral("inputDeadzone"), runtime_settings.value(QStringLiteral("input/deadzone"), 0.10));
  config.insert(QStringLiteral("inputRumble"), runtime_settings.value(QStringLiteral("input/rumble"), true));
  config.insert(QStringLiteral("audioMasterVolume"), runtime_settings.value(QStringLiteral("audio/masterVolume"), 1.0));
  config.insert(QStringLiteral("audioMuteUnfocused"), runtime_settings.value(QStringLiteral("audio/muteUnfocused"), false));
  config.insert(QStringLiteral("audioLatencyProfile"), runtime_settings.value(QStringLiteral("audio/latencyProfile"), QStringLiteral("Automatic")));
  config.insert(QStringLiteral("gameRoot"), profile.value(QStringLiteral("effectiveGamePath"), paths_.configuredPath(QStringLiteral("games"))));
  config.insert(QStringLiteral("managedGamePath"), library_.managedPath(game_id));
  config.insert(QStringLiteral("dlc"), dlc_.launchEntries(game_id));
  const auto save_root = profile.value(QStringLiteral("effectiveSavePath"),
                                       paths_.configuredPath(QStringLiteral("saves"))).toString();
  const auto screenshots_root = profile.value(
      QStringLiteral("effectiveScreenshotPath"),
      paths_.configuredPath(QStringLiteral("screenshots"))).toString();
  config.insert(QStringLiteral("savePath"), QDir{save_root}.filePath(game_id));
  config.insert(QStringLiteral("screenshotsPath"), QDir{screenshots_root}.filePath(game_id));
  config.insert(QStringLiteral("offline"), profile.value(QStringLiteral("offline"), true));
  config.insert(QStringLiteral("isolatedSettings"), profile.value(QStringLiteral("isolatedSettings"), false));
  config.insert(QStringLiteral("fixture"), true);
  return config;
}

ServiceResult LaunchFeature::validate(const QString& game_id) const {
  if (!test_mode_) return launch_.validate(game_id);
  const auto game = library_.entry(game_id);
  if (game.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Launch"),
                                  QStringLiteral("The selected fixture game no longer exists."));
  }
  if (!game.value(QStringLiteral("ready")).toBool()) {
    return ServiceResult::failure(QStringLiteral("Launch unavailable"),
                                  QStringLiteral("The selected fixture intentionally represents missing content."));
  }
  const auto module_id = game.value(QStringLiteral("moduleId")).toString();
  const auto module = modules_.module(module_id);
  if (module.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module missing"),
                                  QStringLiteral("The fixture game does not have its declared module available."));
  }
  if (!module.value(QStringLiteral("active")).toBool()) {
    return ServiceResult::failure(QStringLiteral("Module disabled"),
                                  QStringLiteral("Enable the fixture game module before launching."));
  }
  if (configurationFor(game_id).isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Launch"),
                                  QStringLiteral("The fixture launch configuration could not be assembled."));
  }
  return ServiceResult::success(QStringLiteral("Fixture launch validated"),
                                QStringLiteral("The fixture launch contract passed launcher-side validation."));
}

ServiceResult LaunchFeature::startValidated(const QString& game_id) {
  if (!test_mode_) return launch_.startValidated(game_id);
  return ServiceResult::failure(
      QStringLiteral("Fixture runtime execution unavailable"),
      QStringLiteral("The fixture launch contract is valid, but test data does not represent executable Xbox 360 content and is never reported as a running Xenon session."),
      configurationFor(game_id));
}

ServiceResult LaunchFeature::launch(const QString& game_id) {
  const auto validation = validate(game_id);
  if (!validation.ok) return validation;
  return startValidated(game_id);
}

ServiceResult LaunchFeature::stop() {
  if (!test_mode_) return launch_.stop();
  return ServiceResult::success(QStringLiteral("Fixture session stopped"));
}

QVariantMap LaunchFeature::runtimeStatus() const {
  if (!test_mode_) return launch_.runtimeStatus();
  return {};
}

QString LaunchFeature::afterLaunchBehavior() const {
  return settings_.stringValue(QStringLiteral("runtime/afterLaunch"), QStringLiteral("Keep launcher open"));
}

}  // namespace xenon::launcher::frontend_backend
