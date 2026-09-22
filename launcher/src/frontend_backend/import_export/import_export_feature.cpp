#include "import_export_feature.hpp"

#include "../../services/content_import_service.hpp"
#include "../library/library_feature.hpp"
#include "../modules/modules_feature.hpp"
#include "../profiles/profiles_feature.hpp"

namespace xenon::launcher::frontend_backend {

ImportExportFeature::ImportExportFeature(ContentImportService& content_import,
                                         ProfilesFeature& profiles, LibraryFeature& library,
                                         ModulesFeature& modules, bool test_mode)
    : content_import_(content_import), profiles_(profiles), library_(library),
      modules_(modules), test_mode_(test_mode) {}

ServiceResult ImportExportFeature::importGameContent(const QList<QUrl>& sources, bool moveIntoLibrary) {
  if (!test_mode_) return content_import_.importGameContent(sources, moveIntoLibrary);
  if (sources.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Game import"),
                                  QStringLiteral("No game content was selected."));
  }
  return ServiceResult::success(
      QStringLiteral("Fixture game import"),
      QStringLiteral("The importer boundary received %1 test item(s). No production library data was changed.")
          .arg(sources.size()));
}

ServiceResult ImportExportFeature::importDlc(const QString& game_id, const QList<QUrl>& sources,
                                                    const QString& expected_dlc_id) {
  if (!test_mode_) return content_import_.importDlcContent(game_id, sources, expected_dlc_id);
  if (library_.entry(game_id).isEmpty()) {
    return ServiceResult::failure(QStringLiteral("DLC import"),
                                  QStringLiteral("The selected fixture game no longer exists."));
  }
  if (sources.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("DLC import"),
                                  QStringLiteral("No add-on content was selected."));
  }
  if (!expected_dlc_id.trimmed().isEmpty()) {
    // The fixture catalogue is not mutated, but the targeted workflow still
    // preserves the same explicit ID contract as production.
    const auto game = library_.entry(game_id);
    if (game.isEmpty()) {
      return ServiceResult::failure(QStringLiteral("DLC import"),
                                    QStringLiteral("The selected fixture game no longer exists."));
    }
  }
  return ServiceResult::success(
      QStringLiteral("Fixture DLC import"),
      QStringLiteral("The importer boundary received %1 test item(s). No production files were changed.")
          .arg(sources.size()));
}

ServiceResult ImportExportFeature::importModulePackages(const QList<QUrl>& sources) {
  return modules_.importPackages(sources);
}

ServiceResult ImportExportFeature::exportProfile(int index, const QUrl& destination) const {
  return profiles_.exportProfile(index, destination);
}

ServiceResult ImportExportFeature::importProfile(const QUrl& source) {
  return profiles_.importProfile(source);
}

ServiceResult ImportExportFeature::importProfileAvatar(const QString& profile_id, const QUrl& source) {
  return profiles_.importAvatar(profile_id, source);
}

bool ImportExportFeature::removeProfileAvatar(const QString& profile_id) {
  return profiles_.removeAvatar(profile_id);
}

}  // namespace xenon::launcher::frontend_backend
