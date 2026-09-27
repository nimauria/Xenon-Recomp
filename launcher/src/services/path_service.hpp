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
  // The user-configurable master game-library root (Documents/Xenon Launcher
  // by default - see docs/development/GAME_PREPARATION.md "Managed game library"). Games,
  // saves and profiles default to subfolders of this ONE root so a user who
  // keeps their library on another drive only has to change one setting.
  // Disposable/generated data (native-module cache, preparation staging,
  // logs, shader cache) never lives here - see cachePath()/preparationCachePath().
  [[nodiscard]] QString defaultLibraryRootPath() const;
  [[nodiscard]] QString libraryRootPath() const;
  // Where the automatic game-preparation pipeline (tools/xenon_prepare.cpp)
  // stages generated source, nested builds, and the prepared native-module
  // artifact cache. Always under cachePath(), deliberately never under the
  // library root - a prepared module is disposable/regeneratable, not user
  // data, and must not clutter or bloat a Documents-based library folder.
  [[nodiscard]] QString preparationCachePath() const;
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
