#include "path_service.hpp"

#include "settings_service.hpp"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace xenon::launcher {

PathService::PathService(SettingsService& settings) : settings_(settings) {}

QString PathService::appDataPath() const {
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString PathService::configPath() const {
  return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString PathService::cachePath() const {
  return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

QString PathService::defaultGameLibraryPath() const {
  const auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  return QDir{documents}.filePath(QStringLiteral("Xenon/Games"));
}

QString PathService::defaultSaveDataPath() const {
  const auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  return QDir{documents}.filePath(QStringLiteral("Xenon/Saves"));
}

QString PathService::defaultProfilesPath() const {
  return QDir{appDataPath()}.filePath(QStringLiteral("Profiles"));
}

QString PathService::defaultModulesPath() const {
  return QDir{appDataPath()}.filePath(QStringLiteral("Modules"));
}

QString PathService::defaultScreenshotsPath() const {
  const auto pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
  return QDir{pictures}.filePath(QStringLiteral("Xenon"));
}

QString PathService::packagesPath() const {
  return QDir{appDataPath()}.filePath(QStringLiteral("Packages"));
}

QString PathService::libraryMetadataPath() const {
  return QDir{appDataPath()}.filePath(QStringLiteral("Library/library.json"));
}

QString PathService::configuredPath(const QString& id) const {
  const auto key = QStringLiteral("frontend/paths/") + id;
  if (id == QStringLiteral("games")) return settings_.stringValue(key, defaultGameLibraryPath());
  if (id == QStringLiteral("saves")) return settings_.stringValue(key, defaultSaveDataPath());
  if (id == QStringLiteral("profiles")) return settings_.stringValue(key, defaultProfilesPath());
  if (id == QStringLiteral("modules")) return settings_.stringValue(key, defaultModulesPath());
  if (id == QStringLiteral("screenshots")) return settings_.stringValue(key, defaultScreenshotsPath());
  if (id == QStringLiteral("cache")) return settings_.stringValue(key, cachePath());
  return {};
}

bool PathService::ensureDirectory(const QString& path) const {
  const auto clean = QDir::cleanPath(path.trimmed());
  return !clean.isEmpty() && (QDir{clean}.exists() || QDir{}.mkpath(clean));
}

bool PathService::ensureLauncherDirectories() const {
  return ensureDirectory(appDataPath()) && ensureDirectory(configPath()) &&
         ensureDirectory(cachePath()) && ensureDirectory(defaultProfilesPath()) &&
         ensureDirectory(defaultModulesPath()) && ensureDirectory(packagesPath()) &&
         ensureDirectory(QFileInfo{libraryMetadataPath()}.absolutePath());
}

}  // namespace xenon::launcher
