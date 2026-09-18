#pragma once

#include <QString>

namespace xenon::launcher {

class SettingsService;

class PathService final {
 public:
  explicit PathService(SettingsService& settings);

  [[nodiscard]] QString appDataPath() const;
  [[nodiscard]] QString configPath() const;
  [[nodiscard]] QString cachePath() const;
  [[nodiscard]] QString defaultGameLibraryPath() const;
  [[nodiscard]] QString defaultSaveDataPath() const;
  [[nodiscard]] QString defaultProfilesPath() const;
  [[nodiscard]] QString defaultModulesPath() const;
  [[nodiscard]] QString defaultScreenshotsPath() const;
  [[nodiscard]] QString packagesPath() const;
  [[nodiscard]] QString libraryMetadataPath() const;

  [[nodiscard]] QString configuredPath(const QString& id) const;
  [[nodiscard]] bool ensureDirectory(const QString& path) const;
  [[nodiscard]] bool ensureLauncherDirectories() const;

 private:
  SettingsService& settings_;
};

}  // namespace xenon::launcher
