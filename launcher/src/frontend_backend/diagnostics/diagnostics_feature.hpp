#pragma once

#include "support/support_bundle_service.hpp"

#include <QString>

namespace xenon::launcher {
class PathService;
}

namespace xenon::launcher::frontend_backend {
class ApplicationFeature;
class AppearanceFeature;
class LibraryFeature;
class InputFeature;
class ModulesFeature;
class ProfilesFeature;
class RecoveryFeature;
class RuntimeFeature;
class SessionController;
class SettingsFeature;

class DiagnosticsFeature final {
 public:
  DiagnosticsFeature(PathService& paths, ApplicationFeature& application, RecoveryFeature& recovery,
                     AppearanceFeature& appearance, RuntimeFeature& runtime, InputFeature& input, ProfilesFeature& profiles,
                     SettingsFeature& settings, LibraryFeature& library, ModulesFeature& modules,
                     SessionController& session);

  [[nodiscard]] QString hostArchitecture() const;
  [[nodiscard]] QString platformName() const;
  [[nodiscard]] QString qtVersion() const;
  [[nodiscard]] QString developerDiagnostics() const;
  [[nodiscard]] QString userDiagnostics() const;
  [[nodiscard]] ServiceResult createSupportBundle() const;
  [[nodiscard]] QString supportBundleDirectory() const;
  [[nodiscard]] QString diagnosticsDirectory() const;
  [[nodiscard]] QString startupLogPath() const;

 private:
  [[nodiscard]] QString version() const;

  ApplicationFeature& application_;
  RecoveryFeature& recovery_;
  AppearanceFeature& appearance_;
  RuntimeFeature& runtime_;
  InputFeature& input_;
  ProfilesFeature& profiles_;
  SettingsFeature& settings_;
  SupportBundleService support_bundle_;
};

}  // namespace xenon::launcher::frontend_backend
