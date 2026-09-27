#pragma once

#include "../../services/content_import_service.hpp"
#include "../../services/service_result.hpp"

#include <QUrl>

namespace xenon::launcher::frontend_backend {
class LibraryFeature;
class ModulesFeature;
class ProfilesFeature;

class ImportExportFeature final {
 public:
  ImportExportFeature(ContentImportService& content_import, ProfilesFeature& profiles,
                      LibraryFeature& library, ModulesFeature& modules, bool test_mode);

  [[nodiscard]] ServiceResult importGameContent(const QList<QUrl>& sources, bool moveIntoLibrary = false);
  [[nodiscard]] ServiceResult importDlc(const QString& game_id, const QList<QUrl>& sources,
                                        const QString& expected_dlc_id = {});
  [[nodiscard]] ServiceResult importModulePackages(const QList<QUrl>& sources);
  [[nodiscard]] ServiceResult exportProfile(int index, const QUrl& destination) const;
  [[nodiscard]] ServiceResult importProfile(const QUrl& source);
  [[nodiscard]] ServiceResult importProfileAvatar(const QString& profile_id, const QUrl& source);
  [[nodiscard]] bool removeProfileAvatar(const QString& profile_id);

 private:
  ContentImportService& content_import_;
  ProfilesFeature& profiles_;
  LibraryFeature& library_;
  ModulesFeature& modules_;
  bool test_mode_ = false;
};

}  // namespace xenon::launcher::frontend_backend
