#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include "../../services/service_result.hpp"

namespace xenon::launcher::frontend_backend {
class ApplicationFeature;
class CommunityFeature;
class DiagnosticsFeature;
class LibraryFeature;
class ModulesFeature;
class ProfilesFeature;
class SessionController;
class SettingsFeature;
class UpdateFeature;

class CommandPaletteFeature final : public QObject {
  Q_OBJECT

 public:
  CommandPaletteFeature(ApplicationFeature& application, LibraryFeature& library,
                        ModulesFeature& modules, ProfilesFeature& profiles,
                        SettingsFeature& settings, UpdateFeature& updates,
                        DiagnosticsFeature& diagnostics, CommunityFeature& community,
                        SessionController& session, bool safe_mode,
                        QObject* parent = nullptr);

  [[nodiscard]] QVariantList search(const QString& query, int limit = 24) const;
  [[nodiscard]] ServiceResult execute(const QString& command_id,
                                      const QString& target_id = {},
                                      const QString& section_id = {});

 signals:
  void changed();
  void navigationRequested(int page_index, const QString& target_id,
                           const QString& section_id);

 private:
  [[nodiscard]] QVariantList candidates() const;
  [[nodiscard]] bool settingCategoryAvailable(const QString& category_id) const;

  ApplicationFeature& application_;
  LibraryFeature& library_;
  ModulesFeature& modules_;
  ProfilesFeature& profiles_;
  SettingsFeature& settings_;
  UpdateFeature& updates_;
  DiagnosticsFeature& diagnostics_;
  CommunityFeature& community_;
  SessionController& session_;
  bool safe_mode_ = false;
};

}  // namespace xenon::launcher::frontend_backend
