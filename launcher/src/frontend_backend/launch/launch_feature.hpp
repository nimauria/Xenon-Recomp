#pragma once

#include "../../services/launch_service.hpp"
#include "../../services/service_result.hpp"

#include <QVariantMap>

namespace xenon::launcher::frontend_backend {
class DlcFeature;
class LibraryFeature;
class ModulesFeature;
class PathsFeature;
class ProfilesFeature;
class SettingsFeature;

class LaunchFeature final {
 public:
  LaunchFeature(LaunchService& launch, LibraryFeature& library, DlcFeature& dlc,
                ModulesFeature& modules, ProfilesFeature& profiles, SettingsFeature& settings,
                PathsFeature& paths, bool test_mode);

  void ensureModuleResolved(const QString& game_id) const;
  [[nodiscard]] QVariantMap configurationFor(const QString& game_id) const;
  [[nodiscard]] ServiceResult validate(const QString& game_id) const;
  [[nodiscard]] ServiceResult startValidated(const QString& game_id);
  [[nodiscard]] ServiceResult launch(const QString& game_id);
  [[nodiscard]] ServiceResult stop();
  [[nodiscard]] QVariantMap runtimeStatus() const;
  // "Keep launcher open" / "Minimize launcher" / "Close launcher" - see
  // settings_catalog.cpp's "runtime/afterLaunch" entry. A UI-only preference:
  // it never reaches RuntimeBridge/LaunchConfiguration since the game process
  // is independent of the launcher window regardless of this choice.
  [[nodiscard]] QString afterLaunchBehavior() const;

 private:
  LaunchService& launch_;
  LibraryFeature& library_;
  DlcFeature& dlc_;
  ModulesFeature& modules_;
  ProfilesFeature& profiles_;
  SettingsFeature& settings_;
  PathsFeature& paths_;
  bool test_mode_ = false;
};

}  // namespace xenon::launcher::frontend_backend
