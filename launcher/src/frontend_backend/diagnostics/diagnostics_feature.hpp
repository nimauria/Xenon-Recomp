#pragma once

#include <QString>

namespace xenon::launcher::frontend_backend {
class ApplicationFeature;
class AppearanceFeature;
class ProfilesFeature;
class RuntimeFeature;
class SettingsFeature;

class DiagnosticsFeature final {
 public:
  DiagnosticsFeature(ApplicationFeature& application, AppearanceFeature& appearance,
                     RuntimeFeature& runtime, ProfilesFeature& profiles, SettingsFeature& settings);

  [[nodiscard]] QString hostArchitecture() const;
  [[nodiscard]] QString platformName() const;
  [[nodiscard]] QString qtVersion() const;
  [[nodiscard]] QString developerDiagnostics() const;
  [[nodiscard]] QString userDiagnostics() const;

 private:
  [[nodiscard]] QString version() const;

  ApplicationFeature& application_;
  AppearanceFeature& appearance_;
  RuntimeFeature& runtime_;
  ProfilesFeature& profiles_;
  SettingsFeature& settings_;
};

}  // namespace xenon::launcher::frontend_backend
