#include "frontend_backend.hpp"

#include "../launcher_config.hpp"

#ifndef XENON_LAUNCHER_DISCORD_APPLICATION_ID
#define XENON_LAUNCHER_DISCORD_APPLICATION_ID ""
#endif

namespace xenon::launcher::frontend_backend {

FrontendBackend::FrontendBackend(QObject* parent)
    : QObject(parent),
      core_(nullptr),
      settings_(core_.settings(), nullptr),
      appearance_(core_.settings(), settings_, nullptr),
      paths_(core_.paths(), settings_),
      application_(core_.settings(), nullptr),
      runtime_(core_.runtime()),
      profiles_(core_.profiles(), core_.paths(), core_.library(), core_.modules(), settings_, runtime_, kTestMode, nullptr),
      modules_(core_.modules(), core_.library(), core_.packages(), settings_, kTestMode, nullptr),
      library_(core_.library(), modules_, settings_, kTestMode, nullptr),
      dlc_(core_.dlc(), library_, settings_, kTestMode, nullptr),
      game_properties_(library_, dlc_, modules_, profiles_, paths_),
      import_export_(core_.contentImport(), profiles_, library_, modules_, kTestMode),
      launch_(core_.launch(), library_, dlc_, modules_, profiles_, settings_, paths_, kTestMode),
      session_(launch_, core_.library(), core_.settings(), kTestMode, nullptr),
      filesystem_(core_.paths()),
      community_(settings_, session_, filesystem_, QStringLiteral(XENON_LAUNCHER_DISCORD_APPLICATION_ID), nullptr),
      branding_(),
      diagnostics_(application_, appearance_, runtime_, profiles_, settings_),
      updates_(core_.packages(), settings_, nullptr) {
  connect(&profiles_, &ProfilesFeature::changed, this, [this]() {
    const auto name = profiles_.activeProfile().value(QStringLiteral("profileName")).toString().trimmed();
    if (!name.isEmpty()) core_.settings().setValue(QStringLiteral("profile/name"), name);
  });
}

ServiceResult FrontendBackend::initialize() {
  const auto core_result = core_.initialize(!kTestMode);
  if (!core_result.ok) return core_result;

  const auto primary_name = core_.settings().stringValue(QStringLiteral("profile/name"), QStringLiteral("Nimauria"));
  if (kTestMode) {
    profiles_.initialize(primary_name);
  } else {
    const auto active_name = core_.profiles().activeProfile().value(QStringLiteral("profileName"), primary_name).toString();
    profiles_.initialize(active_name);
  }
  updates_.initialize();
  return ServiceResult::success(QStringLiteral("Launcher core ready"));
}

int FrontendBackend::initialPage() const {
  if (settings_.boolValue(QStringLiteral("general/restoreLastPage"), false)) {
    return application_.initialPage();
  }

  const auto profile_page = profiles_.activeProfile().value(QStringLiteral("startupPage")).toString().trimmed();
  if (profile_page == QStringLiteral("Modules")) return 1;
  if (profile_page == QStringLiteral("Profiles")) return 2;
  if (profile_page == QStringLiteral("Settings")) return 3;
  if (profile_page == QStringLiteral("Library")) return 0;
  return application_.initialPage();
}

ServiceResult FrontendBackend::setPathSetting(const QString& key, const QVariant& value) {
  const auto id = key.mid(QStringLiteral("paths/").size());
  const auto changing_profiles = id == QStringLiteral("profiles");
  const auto changing_modules = id == QStringLiteral("modules");
  const auto previous_path = paths_.configuredPath(id);

  const auto result = paths_.setConfiguredPath(id, value.toString());
  if (!result.ok) return result;

  if (changing_profiles && !kTestMode) {
    const auto persisted = profiles_.persistToCurrentStorage();
    if (!persisted.ok) {
      (void)paths_.setConfiguredPath(id, previous_path);
      return persisted;
    }
    profiles_.reload();
  } else if (id == QStringLiteral("games") || id == QStringLiteral("saves") ||
             id == QStringLiteral("screenshots")) {
    profiles_.refreshDerivedState();
  }

  if (changing_modules) {
    const auto refreshed = modules_.refresh();
    if (!refreshed.ok) {
      (void)paths_.setConfiguredPath(id, previous_path);
      (void)modules_.refresh();
      return refreshed;
    }
  }
  return result;
}

ServiceResult FrontendBackend::setSettingValue(const QString& key, const QVariant& value) {
  const auto normalized = key.trimmed();
  if (normalized.startsWith(QStringLiteral("paths/"))) return setPathSetting(normalized, value);
  if (normalized.startsWith(QStringLiteral("appearance/backdropVariant/"))) {
    const auto theme_id = normalized.mid(QStringLiteral("appearance/backdropVariant/").size());
    return appearance_.setBackgroundVariant(theme_id, value.toString());
  }
  if (normalized == QStringLiteral("runtime/graphicsBackend")) {
    const auto requested = value.toString();
    if (!runtime_.availableGraphicsBackends(kTestMode).contains(requested)) {
      return ServiceResult::failure(
          QStringLiteral("Graphics backend unavailable"),
          QStringLiteral("%1 is not available in this Xenon build on this host.").arg(requested));
    }
  }
  return settings_.setValidatedValue(normalized, value);
}

ServiceResult FrontendBackend::resetSetting(const QString& key) {
  const auto normalized = key.trimmed();
  if (normalized.startsWith(QStringLiteral("paths/"))) {
    const auto id = normalized.mid(QStringLiteral("paths/").size());
    const auto previous_path = paths_.configuredPath(id);
    const auto result = paths_.resetConfiguredPath(id);
    if (!result.ok) return result;
    if (id == QStringLiteral("profiles") && !kTestMode) {
      const auto persisted = profiles_.persistToCurrentStorage();
      if (!persisted.ok) {
        (void)paths_.setConfiguredPath(id, previous_path);
        return persisted;
      }
      profiles_.reload();
    } else if (id == QStringLiteral("games") || id == QStringLiteral("saves") ||
               id == QStringLiteral("screenshots")) {
      profiles_.refreshDerivedState();
    }
    if (id == QStringLiteral("modules")) {
      const auto refreshed = modules_.refresh();
      if (!refreshed.ok) {
        (void)paths_.setConfiguredPath(id, previous_path);
        (void)modules_.refresh();
        return refreshed;
      }
    }
    return result;
  }
  settings_.reset(normalized);
  return ServiceResult::success();
}

ServiceResult FrontendBackend::resetSettingsCategory(const QString& category_id) {
  const auto normalized = category_id.trimmed().toLower();
  if (normalized == QStringLiteral("appearance")) return appearance_.resetAppearance();
  if (normalized == QStringLiteral("paths")) {
    for (const auto& id : {QStringLiteral("games"), QStringLiteral("saves"), QStringLiteral("profiles"),
                           QStringLiteral("modules"), QStringLiteral("screenshots"), QStringLiteral("cache")}) {
      const auto result = resetSetting(QStringLiteral("paths/") + id);
      if (!result.ok) return result;
    }
    return ServiceResult::success(QStringLiteral("Paths reset"),
                                  QStringLiteral("Launcher paths were restored to their platform defaults."));
  }
  return settings_.resetCategory(normalized);
}

ServiceResult FrontendBackend::resetAllSettings() {
  const auto result = settings_.resetAll();
  if (!result.ok) return result;
  // ui/page lives outside the public settings catalog because it is navigation
  // state, but Reset All should still restore the startup-navigation baseline.
  core_.settings().remove(QStringLiteral("ui/page"));
  const auto appearance = appearance_.resetAppearance();
  if (!appearance.ok) return appearance;
  const auto paths = resetSettingsCategory(QStringLiteral("paths"));
  if (!paths.ok) return paths;
  return ServiceResult::success(QStringLiteral("Settings reset"),
                                QStringLiteral("All launcher settings and paths were restored to their defaults."));
}


}  // namespace xenon::launcher::frontend_backend
