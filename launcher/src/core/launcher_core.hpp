#pragma once

#include "../runtime/content_probe.hpp"
#include "../runtime/runtime_bridge.hpp"
#include "../services/content_import_service.hpp"
#include "../services/dlc_service.hpp"
#include "../services/launch_service.hpp"
#include "../services/library_service.hpp"
#include "../services/module_service.hpp"
#include "../services/package_service.hpp"
#include "../services/path_service.hpp"
#include "../services/profile_service.hpp"
#include "../services/settings_service.hpp"

#include <QObject>

namespace xenon::launcher {

class LauncherCore final : public QObject {
  Q_OBJECT

 public:
  explicit LauncherCore(QObject* parent = nullptr);

  [[nodiscard]] ServiceResult initialize(bool load_launcher_state = true);

  [[nodiscard]] SettingsService& settings() noexcept { return settings_; }
  [[nodiscard]] const SettingsService& settings() const noexcept { return settings_; }
  [[nodiscard]] PathService& paths() noexcept { return paths_; }
  [[nodiscard]] const PathService& paths() const noexcept { return paths_; }
  [[nodiscard]] ProfileService& profiles() noexcept { return profiles_; }
  [[nodiscard]] const ProfileService& profiles() const noexcept { return profiles_; }
  [[nodiscard]] LibraryService& library() noexcept { return library_; }
  [[nodiscard]] DlcService& dlc() noexcept { return dlc_; }
  [[nodiscard]] const DlcService& dlc() const noexcept { return dlc_; }
  [[nodiscard]] ContentImportService& contentImport() noexcept { return content_import_; }
  [[nodiscard]] const ContentImportService& contentImport() const noexcept { return content_import_; }
  [[nodiscard]] const LibraryService& library() const noexcept { return library_; }
  [[nodiscard]] ModuleService& modules() noexcept { return modules_; }
  [[nodiscard]] const ModuleService& modules() const noexcept { return modules_; }
  [[nodiscard]] PackageService& packages() noexcept { return packages_; }
  [[nodiscard]] RuntimeBridge& runtime() noexcept { return runtime_; }
  [[nodiscard]] const RuntimeBridge& runtime() const noexcept { return runtime_; }
  [[nodiscard]] LaunchService& launch() noexcept { return launch_; }

 private:
  SettingsService settings_;
  PathService paths_;
  PackageService packages_;
  ProfileService profiles_;
  LibraryService library_;
  ModuleService modules_;
  DlcService dlc_;
  UnavailableContentProbe content_probe_;
  ContentImportService content_import_;
  RuntimeBridge runtime_;
  LaunchService launch_;
};

}  // namespace xenon::launcher
