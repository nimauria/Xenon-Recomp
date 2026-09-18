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

  [[nodiscard]] QVariantMap configurationFor(const QString& game_id) const;
  [[nodiscard]] ServiceResult validate(const QString& game_id) const;
  [[nodiscard]] ServiceResult startValidated(const QString& game_id);
  [[nodiscard]] ServiceResult launch(const QString& game_id);
  [[nodiscard]] ServiceResult stop();

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
