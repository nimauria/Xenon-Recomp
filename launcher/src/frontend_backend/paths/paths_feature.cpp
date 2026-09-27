#include "paths_feature.hpp"

#include "../settings/settings_feature.hpp"

#include <QDir>

namespace xenon::launcher::frontend_backend {

PathsFeature::PathsFeature(PathService& paths, SettingsFeature& settings)
    : paths_(paths), settings_(settings) {}

QString PathsFeature::appDataPath() const { return paths_.appDataPath(); }
QString PathsFeature::configPath() const { return paths_.configPath(); }
QString PathsFeature::cachePath() const { return paths_.cachePath(); }
QString PathsFeature::defaultGameLibraryPath() const { return paths_.defaultGameLibraryPath(); }
QString PathsFeature::defaultSaveDataPath() const { return paths_.defaultSaveDataPath(); }
QString PathsFeature::defaultProfilesPath() const { return paths_.defaultProfilesPath(); }
QString PathsFeature::defaultModulesPath() const { return paths_.defaultModulesPath(); }
QString PathsFeature::defaultScreenshotsPath() const { return paths_.defaultScreenshotsPath(); }
QString PathsFeature::configuredPath(const QString& id) const { return paths_.configuredPath(id); }
bool PathsFeature::ensureDirectory(const QString& path) const { return paths_.ensureDirectory(path); }

bool PathsFeature::validId(const QString& id) {
  return id == QStringLiteral("games") || id == QStringLiteral("saves") ||
         id == QStringLiteral("profiles") || id == QStringLiteral("modules") ||
         id == QStringLiteral("screenshots") || id == QStringLiteral("cache");
}

ServiceResult PathsFeature::setConfiguredPath(const QString& id, const QString& path) {
  const auto normalized_id = id.trimmed();
  if (!validId(normalized_id)) {
    return ServiceResult::failure(QStringLiteral("Path not changed"),
                                  QStringLiteral("The requested launcher path is not recognized."));
  }

  const auto trimmed = path.trimmed();
  if (trimmed.isEmpty()) {
    settings_.reset(QStringLiteral("paths/") + normalized_id);
    return ServiceResult::success(QStringLiteral("Path reset"),
                                  QStringLiteral("The launcher will use the default %1 path.").arg(normalized_id));
  }

  const auto clean = QDir::cleanPath(trimmed);
  if (!paths_.ensureDirectory(clean)) {
    return ServiceResult::failure(QStringLiteral("Path not changed"),
                                  QStringLiteral("Xenon could not create or access:\n%1").arg(clean));
  }
  settings_.setValue(QStringLiteral("paths/") + normalized_id, clean);
  return ServiceResult::success(QStringLiteral("Path updated"),
                                QStringLiteral("The %1 path is now:\n%2").arg(normalized_id, clean), clean);
}

ServiceResult PathsFeature::resetConfiguredPath(const QString& id) {
  if (!validId(id.trimmed())) {
    return ServiceResult::failure(QStringLiteral("Path not reset"),
                                  QStringLiteral("The requested launcher path is not recognized."));
  }
  settings_.reset(QStringLiteral("paths/") + id.trimmed());
  return ServiceResult::success(QStringLiteral("Path reset"));
}

}  // namespace xenon::launcher::frontend_backend
