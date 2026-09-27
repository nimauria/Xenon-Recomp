#pragma once

#include "../../services/path_service.hpp"
#include "../../services/service_result.hpp"

#include <QString>

namespace xenon::launcher::frontend_backend {

class SettingsFeature;

class PathsFeature final {
 public:
  PathsFeature(PathService& paths, SettingsFeature& settings);

  [[nodiscard]] QString appDataPath() const;
  [[nodiscard]] QString configPath() const;
  [[nodiscard]] QString cachePath() const;
  [[nodiscard]] QString defaultGameLibraryPath() const;
  [[nodiscard]] QString defaultSaveDataPath() const;
  [[nodiscard]] QString defaultProfilesPath() const;
  [[nodiscard]] QString defaultModulesPath() const;
  [[nodiscard]] QString defaultScreenshotsPath() const;
  [[nodiscard]] QString configuredPath(const QString& id) const;

  [[nodiscard]] ServiceResult setConfiguredPath(const QString& id, const QString& path);
  [[nodiscard]] ServiceResult resetConfiguredPath(const QString& id);
  [[nodiscard]] bool ensureDirectory(const QString& path) const;

 private:
  [[nodiscard]] static bool validId(const QString& id);

  PathService& paths_;
  SettingsFeature& settings_;
};

}  // namespace xenon::launcher::frontend_backend
