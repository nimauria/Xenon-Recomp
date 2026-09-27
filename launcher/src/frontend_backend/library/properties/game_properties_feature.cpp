#include "game_properties_feature.hpp"

#include "../dlc/dlc_feature.hpp"
#include "../library_feature.hpp"
#include "../../modules/modules_feature.hpp"
#include "../../paths/paths_feature.hpp"
#include "../../profiles/profiles_feature.hpp"

#include <QDir>
#include <QFileInfo>

namespace xenon::launcher::frontend_backend {

GamePropertiesFeature::GamePropertiesFeature(LibraryFeature& library, DlcFeature& dlc,
                                             ModulesFeature& modules, ProfilesFeature& profiles,
                                             PathsFeature& paths)
    : library_(library), dlc_(dlc), modules_(modules), profiles_(profiles), paths_(paths) {}

QVariantMap GamePropertiesFeature::properties(const QString& game_id) const {
  auto game = library_.entry(game_id);
  if (game.isEmpty()) return {};

  const auto profile = profiles_.activeProfile();
  const auto module = modules_.module(game.value(QStringLiteral("moduleId")).toString());
  const auto dlc_entries = dlc_.entries(game_id);
  int installed_dlc = 0;
  for (const auto& value : dlc_entries) {
    if (value.toMap().value(QStringLiteral("installed")).toBool()) ++installed_dlc;
  }

  const auto save_root = profile.value(QStringLiteral("effectiveSavePath"),
                                       paths_.configuredPath(QStringLiteral("saves"))).toString();
  const auto screenshot_root = profile.value(QStringLiteral("effectiveScreenshotPath"),
                                             paths_.configuredPath(QStringLiteral("screenshots"))).toString();
  const auto content_path = library_.contentPath(game_id);
  const QFileInfo content_info{content_path};

  game.insert(QStringLiteral("contentPath"), content_path);
  game.insert(QStringLiteral("contentFolder"), library_.contentFolder(game_id));
  game.insert(QStringLiteral("contentExists"), content_info.exists());
  game.insert(QStringLiteral("contentKind"), content_info.isDir() ? QStringLiteral("Directory")
                                                                  : QStringLiteral("File"));
  const auto managed_path = library_.managedPath(game_id);
  const auto dlc_root = dlc_.rootPath(game_id);
  const auto save_path = QDir{save_root}.filePath(game_id);
  const auto screenshots_path = QDir{screenshot_root}.filePath(game_id);
  if (!managed_path.isEmpty()) {
    (void)paths_.ensureDirectory(save_path);
    (void)paths_.ensureDirectory(screenshots_path);
  }
  game.insert(QStringLiteral("managedPath"), managed_path);
  game.insert(QStringLiteral("dlcRoot"), dlc_root);
  game.insert(QStringLiteral("savePath"), save_path);
  game.insert(QStringLiteral("screenshotsPath"), screenshots_path);
  game.insert(QStringLiteral("modulePath"), module.value(QStringLiteral("path")));
  game.insert(QStringLiteral("moduleActive"), module.value(QStringLiteral("active"), false));
  game.insert(QStringLiteral("dlcCount"), dlc_entries.size());
  game.insert(QStringLiteral("installedDlcCount"), installed_dlc);
  game.insert(QStringLiteral("activeProfile"), profile.value(QStringLiteral("profileName")));
  return game;
}

}  // namespace xenon::launcher::frontend_backend
