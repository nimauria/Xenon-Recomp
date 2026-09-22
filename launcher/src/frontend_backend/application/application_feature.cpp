#include "application_feature.hpp"

#include <QGuiApplication>
#include <QStyleHints>
#include <QtGlobal>
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include <QAccessibilityHints>
#endif

namespace xenon::launcher::frontend_backend {

ApplicationFeature::ApplicationFeature(SettingsService& settings, QObject* parent)
    : QObject(parent), settings_(settings) {
  if (auto* hints = QGuiApplication::styleHints()) {
    connect(hints, &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme) { emit systemAppearanceChanged(); });
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    if (const auto* accessibility = hints->accessibility()) {
      connect(accessibility, &QAccessibilityHints::contrastPreferenceChanged, this,
              [this](Qt::ContrastPreference) { emit systemAppearanceChanged(); });
    }
#endif
  }
}

bool ApplicationFeature::systemDark() const noexcept {
  const auto* hints = QGuiApplication::styleHints();
  return hints != nullptr && hints->colorScheme() == Qt::ColorScheme::Dark;
}

bool ApplicationFeature::systemHighContrast() const noexcept {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  const auto* hints = QGuiApplication::styleHints();
  const auto* accessibility = hints != nullptr ? hints->accessibility() : nullptr;
  return accessibility != nullptr &&
         accessibility->contrastPreference() == Qt::ContrastPreference::HighContrast;
#else
  return false;
#endif
}

int ApplicationFeature::rememberedPage() const {
  return settings_.intValue(QStringLiteral("ui/page"), 0);
}

int ApplicationFeature::initialPage() const {
  if (settings_.boolValue(QStringLiteral("frontend/general/restoreLastPage"), false)) {
    const auto max_page = settings_.boolValue(QStringLiteral("developer/modeEnabled"), false) ? 9 : 8;
    return qBound(0, rememberedPage(), max_page);
  }

  const auto startup = settings_.stringValue(QStringLiteral("frontend/general/startupPage"),
                                             QStringLiteral("Library"));
  if (startup == QStringLiteral("Modules")) return 1;
  if (startup == QStringLiteral("Profiles")) return 2;
  if (startup == QStringLiteral("Settings")) return 3;
  if (startup == QStringLiteral("Home")) return 4;
  if (startup == QStringLiteral("Downloads")) return 5;
  if (startup == QStringLiteral("Captures")) return 6;
  if (startup == QStringLiteral("Network")) return 7;
  if (startup == QStringLiteral("Support")) return 8;
  return 0;
}

void ApplicationFeature::rememberPage(int page_index) {
  const auto max_page = settings_.boolValue(QStringLiteral("developer/modeEnabled"), false) ? 9 : 8;
  settings_.setValue(QStringLiteral("ui/page"), qBound(0, page_index, max_page));
}

bool ApplicationFeature::featureEnabled(const QString& feature) const noexcept {
  const auto& f = kUiFeatures;
  if (feature == QStringLiteral("settings.general")) return f.settings_general;
  if (feature == QStringLiteral("settings.system")) return f.settings_system;
  if (feature == QStringLiteral("settings.appearance")) return f.settings_appearance;
  if (feature == QStringLiteral("settings.library")) return f.settings_library;
  if (feature == QStringLiteral("settings.paths")) return f.settings_paths;
  if (feature == QStringLiteral("settings.runtime")) return f.settings_runtime;
  if (feature == QStringLiteral("settings.graphics")) return f.settings_graphics;
  if (feature == QStringLiteral("settings.input")) return f.settings_input;
  if (feature == QStringLiteral("settings.audio")) return f.settings_audio;
  if (feature == QStringLiteral("settings.network")) return f.settings_network;
  if (feature == QStringLiteral("settings.filesystem")) return f.settings_filesystem;
  if (feature == QStringLiteral("settings.updates")) return f.settings_updates;
  if (feature == QStringLiteral("settings.community")) return f.settings_community;
  if (feature == QStringLiteral("settings.accessibility")) return f.settings_accessibility;
  if (feature == QStringLiteral("settings.developer")) return f.settings_developer;
  if (feature == QStringLiteral("settings.about")) return f.settings_about;
  if (feature == QStringLiteral("library.dlc")) return f.library_dlc;
  if (feature == QStringLiteral("library.manageFiles")) return f.library_manage_files;
  if (feature == QStringLiteral("modules.updates")) return f.modules_updates;
  if (feature == QStringLiteral("modules.catalog")) return f.modules_catalog;
  if (feature == QStringLiteral("modules.settings")) return f.modules_settings;
  if (feature == QStringLiteral("profiles.paths")) return f.profiles_paths;
  if (feature == QStringLiteral("profiles.avatars")) return f.profiles_avatars;
  return false;
}

}  // namespace xenon::launcher::frontend_backend
