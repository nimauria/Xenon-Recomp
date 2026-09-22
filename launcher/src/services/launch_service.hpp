#pragma once

#include "../models/launch_configuration.hpp"
#include "service_result.hpp"

#include <optional>
#include <QVariantMap>

namespace xenon::launcher {

class ContentImportService;
class DlcService;
class FilesystemService;
class LibraryService;
class ModuleService;
class PathService;
class PreparationService;
class ProfileService;
class IRuntimeBridge;
class SettingsService;

class LaunchService final {
 public:
  LaunchService(SettingsService& settings, PathService& paths, ProfileService& profiles,
                LibraryService& library, ModuleService& modules, DlcService& dlc,
                FilesystemService& filesystem, IRuntimeBridge& runtime,
                PreparationService& preparation, ContentImportService& content_import);

  // Re-attempts installed-module matching for a game that was identified
  // without one (Part 7) - a no-op if a module is already assigned. Must run
  // before configurationFor()/usesAutomaticPreparation() so a module
  // installed after import is picked up in time for the SAME Play press to
  // trigger automatic preparation, not just the one after it.
  void ensureModuleResolved(const QString& game_id) const;
  [[nodiscard]] std::optional<LaunchConfiguration> configurationFor(const QString& game_id) const;
  [[nodiscard]] ServiceResult validate(const QString& game_id) const;
  [[nodiscard]] ServiceResult startValidated(const QString& game_id);
  [[nodiscard]] ServiceResult launch(const QString& game_id);
  [[nodiscard]] ServiceResult stop();
  [[nodiscard]] QVariantMap runtimeStatus() const;

 private:
  SettingsService& settings_;
  PathService& paths_;
  ProfileService& profiles_;
  LibraryService& library_;
  ModuleService& modules_;
  DlcService& dlc_;
  FilesystemService& filesystem_;
  IRuntimeBridge& runtime_;
  PreparationService& preparation_;
  ContentImportService& content_import_;
};

}  // namespace xenon::launcher
