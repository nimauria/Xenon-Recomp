#pragma once

#include "../core/launcher_core.hpp"
#include "application/application_feature.hpp"
#include "branding/branding_feature.hpp"
#include "community/community_feature.hpp"
#include "diagnostics/diagnostics_feature.hpp"
#include "filesystem/filesystem_feature.hpp"
#include "import_export/import_export_feature.hpp"
#include "launch/launch_feature.hpp"
#include "launch/session/session_controller.hpp"
#include "library/library_feature.hpp"
#include "library/dlc/dlc_feature.hpp"
#include "library/properties/game_properties_feature.hpp"
#include "modules/modules_feature.hpp"
#include "paths/paths_feature.hpp"
#include "profiles/profiles_feature.hpp"
#include "runtime/runtime_feature.hpp"
#include "settings/settings_feature.hpp"
#include "settings/appearance/appearance_feature.hpp"
#include "updates/update_feature.hpp"

#include <QObject>

namespace xenon::launcher::frontend_backend {

class FrontendBackend final : public QObject {
  Q_OBJECT

 public:
  explicit FrontendBackend(QObject* parent = nullptr);

  [[nodiscard]] ServiceResult initialize();
  [[nodiscard]] int initialPage() const;
  [[nodiscard]] ServiceResult setSettingValue(const QString& key, const QVariant& value);
  [[nodiscard]] ServiceResult resetSetting(const QString& key);
  [[nodiscard]] ServiceResult resetSettingsCategory(const QString& category_id);
  [[nodiscard]] ServiceResult resetAllSettings();

  [[nodiscard]] LauncherCore& core() noexcept { return core_; }
  [[nodiscard]] const LauncherCore& core() const noexcept { return core_; }
  [[nodiscard]] ApplicationFeature& application() noexcept { return application_; }
  [[nodiscard]] const ApplicationFeature& application() const noexcept { return application_; }
  [[nodiscard]] SettingsFeature& settings() noexcept { return settings_; }
  [[nodiscard]] const SettingsFeature& settings() const noexcept { return settings_; }
  [[nodiscard]] AppearanceFeature& appearance() noexcept { return appearance_; }
  [[nodiscard]] const AppearanceFeature& appearance() const noexcept { return appearance_; }
  [[nodiscard]] PathsFeature& paths() noexcept { return paths_; }
  [[nodiscard]] const PathsFeature& paths() const noexcept { return paths_; }
  [[nodiscard]] ProfilesFeature& profiles() noexcept { return profiles_; }
  [[nodiscard]] const ProfilesFeature& profiles() const noexcept { return profiles_; }
  [[nodiscard]] LibraryFeature& library() noexcept { return library_; }
  [[nodiscard]] const LibraryFeature& library() const noexcept { return library_; }
  [[nodiscard]] DlcFeature& dlc() noexcept { return dlc_; }
  [[nodiscard]] const DlcFeature& dlc() const noexcept { return dlc_; }
  [[nodiscard]] GamePropertiesFeature& gameProperties() noexcept { return game_properties_; }
  [[nodiscard]] const GamePropertiesFeature& gameProperties() const noexcept { return game_properties_; }
  [[nodiscard]] ModulesFeature& modules() noexcept { return modules_; }
  [[nodiscard]] const ModulesFeature& modules() const noexcept { return modules_; }
  [[nodiscard]] ImportExportFeature& importExport() noexcept { return import_export_; }
  [[nodiscard]] UpdateFeature& updates() noexcept { return updates_; }
  [[nodiscard]] RuntimeFeature& runtime() noexcept { return runtime_; }
  [[nodiscard]] const RuntimeFeature& runtime() const noexcept { return runtime_; }
  [[nodiscard]] LaunchFeature& launch() noexcept { return launch_; }
  [[nodiscard]] SessionController& session() noexcept { return session_; }
  [[nodiscard]] const SessionController& session() const noexcept { return session_; }
  [[nodiscard]] FilesystemFeature& filesystem() noexcept { return filesystem_; }
  [[nodiscard]] const FilesystemFeature& filesystem() const noexcept { return filesystem_; }
  [[nodiscard]] CommunityFeature& community() noexcept { return community_; }
  [[nodiscard]] const CommunityFeature& community() const noexcept { return community_; }
  [[nodiscard]] BrandingFeature& branding() noexcept { return branding_; }
  [[nodiscard]] const BrandingFeature& branding() const noexcept { return branding_; }
  [[nodiscard]] DiagnosticsFeature& diagnostics() noexcept { return diagnostics_; }
  [[nodiscard]] const DiagnosticsFeature& diagnostics() const noexcept { return diagnostics_; }

 private:
  [[nodiscard]] ServiceResult setPathSetting(const QString& key, const QVariant& value);

  LauncherCore core_;
  SettingsFeature settings_;
  AppearanceFeature appearance_;
  PathsFeature paths_;
  ApplicationFeature application_;
  RuntimeFeature runtime_;
  ProfilesFeature profiles_;
  ModulesFeature modules_;
  LibraryFeature library_;
  DlcFeature dlc_;
  GamePropertiesFeature game_properties_;
  ImportExportFeature import_export_;
  LaunchFeature launch_;
  SessionController session_;
  FilesystemFeature filesystem_;
  CommunityFeature community_;
  BrandingFeature branding_;
  DiagnosticsFeature diagnostics_;
  UpdateFeature updates_;
};

}  // namespace xenon::launcher::frontend_backend
