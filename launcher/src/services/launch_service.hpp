#pragma once

#include "../models/launch_configuration.hpp"
#include "service_result.hpp"

#include <optional>

namespace xenon::launcher {

class DlcService;
class FilesystemService;
class LibraryService;
class ModuleService;
class PathService;
class ProfileService;
class IRuntimeBridge;
class SettingsService;

class LaunchService final {
 public:
  LaunchService(SettingsService& settings, PathService& paths, ProfileService& profiles,
                LibraryService& library, ModuleService& modules, DlcService& dlc,
                FilesystemService& filesystem, IRuntimeBridge& runtime);

  [[nodiscard]] std::optional<LaunchConfiguration> configurationFor(const QString& game_id) const;
  [[nodiscard]] ServiceResult validate(const QString& game_id) const;
  [[nodiscard]] ServiceResult startValidated(const QString& game_id);
  [[nodiscard]] ServiceResult launch(const QString& game_id);
  [[nodiscard]] ServiceResult stop();

 private:
  SettingsService& settings_;
  PathService& paths_;
  ProfileService& profiles_;
  LibraryService& library_;
  ModuleService& modules_;
  DlcService& dlc_;
  FilesystemService& filesystem_;
  IRuntimeBridge& runtime_;
};

}  // namespace xenon::launcher
