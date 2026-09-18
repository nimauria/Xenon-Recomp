#include "launcher_bridge.hpp"

#include "launcher_config.hpp"

#include <QSettings>

#ifndef XENON_LAUNCHER_VERSION
#define XENON_LAUNCHER_VERSION "0.0.0-dev"
#endif

namespace {
constexpr auto kSettingsOrganization = "Project Xenon";
constexpr auto kSettingsApplication = "Xenon Launcher";
constexpr auto kDefaultTheme = "xenon-cyan";
constexpr auto kDefaultProfile = "Nimauria";
}  // namespace

LauncherBridge::LauncherBridge(QObject* parent)
    : QObject(parent) {
  QSettings settings{kSettingsOrganization, kSettingsApplication};
  theme_id_ = settings.value("ui/theme", kDefaultTheme).toString();
  profile_name_ = settings.value("profile/name", kDefaultProfile).toString();

  if (theme_id_.isEmpty()) {
    theme_id_ = kDefaultTheme;
  }
  if (profile_name_.isEmpty()) {
    profile_name_ = kDefaultProfile;
  }
}

QString LauncherBridge::version() const {
  return QStringLiteral(XENON_LAUNCHER_VERSION);
}

bool LauncherBridge::backendConnected() const noexcept {
  // The initial launcher milestone is intentionally presentation-only.
  return false;
}

bool LauncherBridge::testMode() const noexcept {
  return xenon::launcher::kTestMode;
}

QString LauncherBridge::themeId() const {
  return theme_id_;
}

void LauncherBridge::setThemeId(const QString& theme_id) {
  if (theme_id.isEmpty() || theme_id == theme_id_) {
    return;
  }

  theme_id_ = theme_id;
  QSettings{kSettingsOrganization, kSettingsApplication}.setValue("ui/theme", theme_id_);
  emit themeIdChanged();
}

QString LauncherBridge::profileName() const {
  return profile_name_;
}

void LauncherBridge::setProfileName(const QString& profile_name) {
  const auto trimmed = profile_name.trimmed();
  if (trimmed.isEmpty() || trimmed == profile_name_) {
    return;
  }

  profile_name_ = trimmed;
  QSettings{kSettingsOrganization, kSettingsApplication}.setValue("profile/name", profile_name_);
  emit profileNameChanged();
}

void LauncherBridge::notifyUnavailable(const QString& feature) {
  emit notificationRequested(
      feature,
      QStringLiteral("The front-end action is wired, but the Xenon backend for this feature is not connected yet."));
}

void LauncherBridge::notify(const QString& title, const QString& message) {
  emit notificationRequested(title, message);
}

void LauncherBridge::rememberPage(int page_index) {
  QSettings{kSettingsOrganization, kSettingsApplication}.setValue("ui/page", page_index);
}

int LauncherBridge::rememberedPage() const {
  return QSettings{kSettingsOrganization, kSettingsApplication}.value("ui/page", 0).toInt();
}
