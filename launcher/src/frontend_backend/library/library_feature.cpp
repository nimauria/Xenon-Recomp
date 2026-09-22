#include "library_feature.hpp"

#include "actions/library_action_catalog.hpp"

#include "../settings/settings_feature.hpp"
#include "../modules/modules_feature.hpp"

#include <QFileInfo>
#include <QStringList>
#include <utility>

namespace xenon::launcher::frontend_backend {
namespace {
QString joined(const QVariant& value, const QString& separator = QStringLiteral(" • ")) {
  QStringList parts;
  for (const auto& item : value.toList()) {
    const auto text = item.toString().trimmed();
    if (!text.isEmpty()) parts.append(text);
  }
  if (parts.isEmpty()) {
    const auto text = value.toString().trimmed();
    if (!text.isEmpty()) parts.append(text);
  }
  return parts.join(separator);
}

QString titleCaseWords(QString value) {
  value = value.trimmed();
  if (value.isEmpty()) return value;
  const auto words = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  QStringList result;
  for (auto word : words) {
    if (!word.isEmpty()) word[0] = word[0].toUpper();
    result.append(word);
  }
  return result.join(QLatin1Char(' '));
}
}  // namespace

LibraryFeature::LibraryFeature(LibraryService& library, ModulesFeature& modules,
                               SettingsFeature& settings, bool test_mode, QObject* parent)
    : QObject(parent), library_(library), modules_(modules), settings_(settings), test_mode_(test_mode) {
  connect(&library_, &LibraryService::changed, this, [this]() {
    if (!test_mode_) emit changed();
  });
  connect(&modules_, &ModulesFeature::changed, this, [this]() { emit changed(); });
  connect(&settings_, &SettingsFeature::changed, this,
          [this](const QString& key, const QVariant&) {
            if (test_mode_ && key == QStringLiteral("developer/fixtureMode")) {
              rebuildFixtures();
              emit changed();
            }
          });
  if (test_mode_) rebuildFixtures();
}

QString LibraryFeature::fixtureMode() const {
  return settings_.stringValue(QStringLiteral("developer/fixtureMode"),
                               test_mode_ ? QStringLiteral("generic") : QStringLiteral("none"));
}

QVariantMap LibraryFeature::gameFixture(QString title, QString module_name, QString status,
                                        bool ready, QString description, QString game_id,
                                        QString module_id, QString renderer, QString regions,
                                        QString content_state, QString tags, QString module_version) {
  QVariantMap item;
  item.insert(QStringLiteral("title"), std::move(title));
  item.insert(QStringLiteral("moduleName"), std::move(module_name));
  item.insert(QStringLiteral("status"), std::move(status));
  item.insert(QStringLiteral("ready"), ready);
  item.insert(QStringLiteral("installed"), true);
  item.insert(QStringLiteral("contentExists"), ready);
  item.insert(QStringLiteral("favorite"), false);
  item.insert(QStringLiteral("moduleInstalled"), true);
  item.insert(QStringLiteral("moduleActive"), true);
  item.insert(QStringLiteral("tileArt"), QString{});
  item.insert(QStringLiteral("heroArt"), QString{});
  item.insert(QStringLiteral("description"), std::move(description));
  item.insert(QStringLiteral("gameId"), std::move(game_id));
  item.insert(QStringLiteral("moduleId"), std::move(module_id));
  item.insert(QStringLiteral("renderer"), std::move(renderer));
  item.insert(QStringLiteral("mode"), QStringLiteral("Offline"));
  item.insert(QStringLiteral("regions"), std::move(regions));
  item.insert(QStringLiteral("contentState"), std::move(content_state));
  item.insert(QStringLiteral("tags"), std::move(tags));
  item.insert(QStringLiteral("moduleVersion"), std::move(module_version));
  item.insert(QStringLiteral("lastPlayed"), QStringLiteral("Not launched"));
  item.insert(QStringLiteral("lastPlayedAt"), QString{});
  item.insert(QStringLiteral("contentPath"), QString{});
  item.insert(QStringLiteral("addedAt"), QString{});
  item.insert(QStringLiteral("identifiedAt"), QString{});
  return item;
}

void LibraryFeature::rebuildFixtures() {
  fixture_entries_.clear();
  const auto mode = fixtureMode();
  if (mode == QStringLiteral("none")) return;
  if (mode == QStringLiteral("gracemeria")) {
    fixture_entries_.append(gameFixture(
        QStringLiteral("Ace Combat 6: Fires of Liberation"), QStringLiteral("Project Gracemeria"),
        QStringLiteral("UI Preview"), true,
        QStringLiteral("Project Gracemeria UI preview for the Xenon launcher. The module supplies metadata, compatibility rules and add-on definitions; users provide their own legally obtained game content locally."),
        QStringLiteral("4E4D07D1"), QStringLiteral("org.nimauria.project-gracemeria"), QStringLiteral("Vulkan"),
        QStringLiteral("NTSC-U / PAL"),
        QStringLiteral("Base content + title update + add-ons detected (preview)"),
        QStringLiteral("Aerial Combat|Cinematic Story|Project Gracemeria"), QStringLiteral("preview")));
    return;
  }

  fixture_entries_.append(gameFixture(
      QStringLiteral("Xenon Test Flight"), QStringLiteral("Test Flight Module"),
      QStringLiteral("Ready to Play"), true,
      QStringLiteral("A fictional launcher-only entry used to exercise the Project Xenon library interface before runtime services are connected."),
      QStringLiteral("TEST0001"), QStringLiteral("xenon.test.flight"), QStringLiteral("Vulkan"),
      QStringLiteral("Test Region A / B"), QStringLiteral("Base content + update + add-ons detected"),
      QStringLiteral("Aerial Combat|Cinematic Story|Large Battles"), QStringLiteral("0.1-test")));
  fixture_entries_.append(gameFixture(
      QStringLiteral("Xenon Test Arena"), QStringLiteral("Test Arena Module"),
      QStringLiteral("Content Missing"), false,
      QStringLiteral("A second fictional entry used to verify disabled launch states and incomplete-content indicators."),
      QStringLiteral("TEST0002"), QStringLiteral("xenon.test.arena"), QStringLiteral("Automatic"),
      QStringLiteral("Module Defined"), QStringLiteral("Required base content missing"),
      QStringLiteral("Test Fixture|Content Validation"), QStringLiteral("0.2-test")));
}

QVariantMap LibraryFeature::withCatalogPresentation(QVariantMap item) const {
  const auto module_id = item.value(QStringLiteral("moduleId")).toString();
  if (module_id.isEmpty()) return item;

  const auto catalog = modules_.catalogEntryData(module_id);
  if (catalog.isEmpty()) {
    item.insert(QStringLiteral("metadataAvailable"), false);
    return item;
  }

  const auto launcher = catalog.value(QStringLiteral("launcher")).toMap();
  const auto game = catalog.value(QStringLiteral("game")).toMap();
  const auto compatibility = launcher.value(QStringLiteral("compatibility")).toMap();

  const auto title = launcher.value(QStringLiteral("gameTitle"), game.value(QStringLiteral("title"))).toString().trimmed();
  if (!title.isEmpty()) item.insert(QStringLiteral("title"), title);
  const auto module_name = launcher.value(QStringLiteral("moduleName"), catalog.value(QStringLiteral("moduleName"))).toString().trimmed();
  if (!module_name.isEmpty()) item.insert(QStringLiteral("moduleName"), module_name);
  const auto description = launcher.value(QStringLiteral("description")).toString().trimmed();
  if (!description.isEmpty()) item.insert(QStringLiteral("description"), description);
  const auto renderer = launcher.value(QStringLiteral("renderer")).toString().trimmed();
  if (!renderer.isEmpty()) item.insert(QStringLiteral("renderer"), renderer);
  const auto mode = launcher.value(QStringLiteral("mode")).toString().trimmed();
  if (!mode.isEmpty()) item.insert(QStringLiteral("mode"), mode);

  auto regions = joined(launcher.value(QStringLiteral("regions")));
  if (regions.isEmpty()) regions = joined(game.value(QStringLiteral("supportedRegions")));
  if (!regions.isEmpty()) item.insert(QStringLiteral("regions"), regions);

  const auto content_state = launcher.value(QStringLiteral("contentStateLabel")).toString().trimmed();
  if (!content_state.isEmpty()) item.insert(QStringLiteral("contentState"), content_state);

  const auto tile = launcher.value(QStringLiteral("tileArtLocalUrl")).toString().trimmed();
  const auto hero = launcher.value(QStringLiteral("heroArtLocalUrl")).toString().trimmed();
  if (!tile.isEmpty()) item.insert(QStringLiteral("tileArt"), tile);
  if (!hero.isEmpty()) item.insert(QStringLiteral("heroArt"), hero);

  QStringList tags;
  for (const auto& genre : game.value(QStringLiteral("genres")).toList()) {
    const auto text = titleCaseWords(genre.toString());
    if (!text.isEmpty() && !tags.contains(text)) tags.append(text);
  }
  if (!module_name.isEmpty() && !tags.contains(module_name)) tags.append(module_name);
  if (!tags.isEmpty()) item.insert(QStringLiteral("tags"), tags.join(QLatin1Char('|')));

  const auto registry_title_id = game.value(QStringLiteral("titleId")).toString().trimmed();
  if (item.value(QStringLiteral("titleId")).toString().trimmed().isEmpty() &&
      !registry_title_id.isEmpty()) {
    item.insert(QStringLiteral("titleId"), registry_title_id);
  }
  item.insert(QStringLiteral("gameDeveloper"), game.value(QStringLiteral("developer")));
  item.insert(QStringLiteral("gamePublisher"), game.value(QStringLiteral("publisher")));
  item.insert(QStringLiteral("gamePlatform"), game.value(QStringLiteral("platform")));
  item.insert(QStringLiteral("releaseYear"), game.value(QStringLiteral("releaseYear")));
  item.insert(QStringLiteral("compatibilityStatus"), compatibility.value(QStringLiteral("status")));
  item.insert(QStringLiteral("compatibilityLabel"), compatibility.value(QStringLiteral("label")));
  item.insert(QStringLiteral("compatibilitySummary"), compatibility.value(QStringLiteral("summary")));
  item.insert(QStringLiteral("metadataAvailable"), true);
  item.insert(QStringLiteral("metadataSource"), QStringLiteral("Official Xenon Modules registry"));
  item.insert(QStringLiteral("metadataEntryUrl"), catalog.value(QStringLiteral("registryEntryUrl")));
  item.insert(QStringLiteral("metadataRepository"), catalog.value(QStringLiteral("repository")));
  const auto catalog_state = modules_.catalogState();
  item.insert(QStringLiteral("metadataStatus"), catalog_state.value(QStringLiteral("status")));
  item.insert(QStringLiteral("metadataStatusMessage"), catalog_state.value(QStringLiteral("message")));
  item.insert(QStringLiteral("metadataLastCheckedAt"), catalog_state.value(QStringLiteral("lastCheckedAt")));
  item.insert(QStringLiteral("metadataAssetState"), launcher.value(QStringLiteral("assetCacheState")));
  return item;
}

QVariantMap LibraryFeature::projected(QVariantMap item) const {
  item = withCatalogPresentation(std::move(item));
  const auto module_id = item.value(QStringLiteral("moduleId")).toString();
  const auto module = modules_.module(module_id);
  const auto module_installed = !module.isEmpty();
  const auto module_active = module_installed && module.value(QStringLiteral("active")).toBool();
  const auto content_path = item.value(QStringLiteral("contentPath")).toString();
  const auto content_exists = !content_path.isEmpty() && QFileInfo::exists(content_path);
  const auto identified = !module_id.isEmpty();

  item.insert(QStringLiteral("moduleInstalled"), module_installed);
  item.insert(QStringLiteral("moduleActive"), module_active);
  item.insert(QStringLiteral("installed"), module_installed);
  item.insert(QStringLiteral("contentExists"), content_exists);

  const auto update_state = modules_.updateState(module_id);
  const auto update_status = update_state.value(QStringLiteral("status"), QStringLiteral("idle")).toString();
  item.insert(QStringLiteral("moduleUpdateStatus"), update_status);
  item.insert(QStringLiteral("moduleUpdateAvailable"),
              update_state.value(QStringLiteral("updateAvailable")).toBool() ||
                  update_state.value(QStringLiteral("canDownload")).toBool() ||
                  update_state.value(QStringLiteral("canInstall")).toBool() ||
                  update_status == QStringLiteral("update-available") ||
                  update_status == QStringLiteral("ready-to-install"));

  if (!identified) {
    item.insert(QStringLiteral("ready"), false);
    item.insert(QStringLiteral("status"), QStringLiteral("Module required"));
  } else if (!module_installed) {
    item.insert(QStringLiteral("ready"), false);
    item.insert(QStringLiteral("status"), QStringLiteral("Module missing"));
  } else if (!module_active) {
    item.insert(QStringLiteral("ready"), false);
    item.insert(QStringLiteral("status"), QStringLiteral("Module disabled"));
  } else if (!content_exists) {
    item.insert(QStringLiteral("ready"), false);
    item.insert(QStringLiteral("status"), QStringLiteral("Content missing"));
  }
  return item;
}

QVariantList LibraryFeature::entries() const {
  QVariantList result;
  if (test_mode_) {
    for (const auto& value : fixture_entries_) {
      auto item = withCatalogPresentation(value.toMap());
      const auto module_id = item.value(QStringLiteral("moduleId")).toString();
      const auto module = modules_.module(module_id);
      const auto module_installed = !module.isEmpty();
      const auto module_active = module_installed && module.value(QStringLiteral("active")).toBool();
      item.insert(QStringLiteral("moduleInstalled"), module_installed);
      item.insert(QStringLiteral("moduleActive"), module_active);
      item.insert(QStringLiteral("installed"), module_installed);
      const auto update_state = modules_.updateState(module_id);
      const auto update_status = update_state.value(QStringLiteral("status"), QStringLiteral("idle")).toString();
      item.insert(QStringLiteral("moduleUpdateStatus"), update_status);
      item.insert(QStringLiteral("moduleUpdateAvailable"),
                  update_state.value(QStringLiteral("updateAvailable")).toBool() ||
                      update_state.value(QStringLiteral("canDownload")).toBool() ||
                      update_state.value(QStringLiteral("canInstall")).toBool() ||
                      update_status == QStringLiteral("update-available") ||
                      update_status == QStringLiteral("ready-to-install"));
      if (!module_installed) {
        item.insert(QStringLiteral("ready"), false);
        item.insert(QStringLiteral("status"), QStringLiteral("Module missing"));
      } else if (!module_active) {
        item.insert(QStringLiteral("ready"), false);
        item.insert(QStringLiteral("status"), QStringLiteral("Module disabled"));
      }
      result.append(item);
    }
    return result;
  }
  for (const auto& value : library_.entries()) result.append(projected(value.toMap()));
  return result;
}

QVariantMap LibraryFeature::entry(const QString& game_id) const {
  if (!test_mode_) return projected(library_.entry(game_id));
  for (const auto& value : entries()) {
    const auto item = value.toMap();
    if (item.value(QStringLiteral("gameId")).toString() == game_id) return item;
  }
  return {};
}

QVariantList LibraryFeature::actions(const QString& game_id) const {
  return LibraryActionCatalog::gameActions(entry(game_id), test_mode_);
}

QVariantList LibraryFeature::manageActions(const QString& game_id) const {
  return LibraryActionCatalog::manageActions(entry(game_id), test_mode_);
}

QVariantList LibraryFeature::backgroundActions() const {
  return LibraryActionCatalog::backgroundActions();
}

ServiceResult LibraryFeature::remove(const QString& game_id) {
  if (!test_mode_) return library_.remove(game_id);
  for (qsizetype i = 0; i < fixture_entries_.size(); ++i) {
    if (fixture_entries_.at(i).toMap().value(QStringLiteral("gameId")).toString() == game_id) {
      fixture_entries_.removeAt(i);
      emit changed();
      return ServiceResult::success(QStringLiteral("Library entry removed"),
                                    QStringLiteral("The fixture entry was removed from this test session."));
    }
  }
  return ServiceResult::failure(QStringLiteral("Library entry"),
                                QStringLiteral("The selected entry no longer exists."));
}

ServiceResult LibraryFeature::setFavorite(const QString& game_id, bool favorite) {
  if (!test_mode_) return library_.setFavorite(game_id, favorite);
  for (qsizetype i = 0; i < fixture_entries_.size(); ++i) {
    auto item = fixture_entries_.at(i).toMap();
    if (item.value(QStringLiteral("gameId")).toString() != game_id) continue;
    item.insert(QStringLiteral("favorite"), favorite);
    fixture_entries_[i] = item;
    emit changed();
    return ServiceResult::success(
        favorite ? QStringLiteral("Added to favorites") : QStringLiteral("Removed from favorites"),
        item.value(QStringLiteral("title"), QStringLiteral("Game")).toString());
  }
  return ServiceResult::failure(QStringLiteral("Favorite"),
                                QStringLiteral("The selected fixture entry no longer exists."));
}

ServiceResult LibraryFeature::deleteManagedFiles(const QString& game_id) {
  if (test_mode_) {
    if (entry(game_id).isEmpty()) {
      return ServiceResult::failure(QStringLiteral("Delete managed files"),
                                    QStringLiteral("The selected fixture entry no longer exists."));
    }
    return ServiceResult::success(QStringLiteral("Managed files deleted"),
                                  QStringLiteral("Fixture entries have no real managed files to delete."));
  }
  return library_.deleteManagedFiles(game_id);
}

ServiceResult LibraryFeature::verify(const QString& game_id) const {
  if (test_mode_) {
    if (entry(game_id).isEmpty()) {
      return ServiceResult::failure(QStringLiteral("Content verification"),
                                    QStringLiteral("The selected fixture entry no longer exists."));
    }
    return ServiceResult::success(QStringLiteral("Fixture content verified"),
                                  QStringLiteral("The launcher-side fixture metadata is internally consistent."));
  }

  const auto game = entry(game_id);
  if (game.isEmpty()) return library_.verify(game_id);
  const auto content_result = library_.verify(game_id);
  if (!content_result.ok) return content_result;
  const auto module_id = game.value(QStringLiteral("moduleId")).toString();
  if (module_id.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Content verification"),
                                  QStringLiteral("The content path exists, but no game module has identified it yet."));
  }
  const auto module_result = modules_.verify(module_id);
  if (!module_result.ok) return module_result;
  return ServiceResult::success(
      QStringLiteral("Library entry verified"),
      QStringLiteral("The registered content path and selected module manifest are both available. Full Xbox 360 content validation will run through the framework content probe once connected."));
}

ServiceResult LibraryFeature::refreshMetadata(const QString& game_id) {
  const auto game = entry(game_id);
  if (game.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Game metadata refresh"),
                                  QStringLiteral("The selected library entry no longer exists."));
  }
  const auto module_id = game.value(QStringLiteral("moduleId")).toString();
  if (module_id.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Game metadata refresh"),
                                  QStringLiteral("The selected game does not have an assigned module."));
  }
  return modules_.refreshPresentationMetadata(module_id);
}

QString LibraryFeature::contentPath(const QString& game_id) const {
  return test_mode_ ? entry(game_id).value(QStringLiteral("contentPath")).toString()
                    : library_.contentPath(game_id);
}

QString LibraryFeature::contentFolder(const QString& game_id) const {
  if (!test_mode_) return library_.contentFolder(game_id);
  const QFileInfo info{contentPath(game_id)};
  return info.isDir() ? info.absoluteFilePath() : info.absolutePath();
}

QString LibraryFeature::managedPath(const QString& game_id) const {
  return test_mode_ ? QString{} : library_.ensureManagedPath(game_id);
}

void LibraryFeature::reload() {
  if (test_mode_) rebuildFixtures();
  else library_.reload();
  emit changed();
}

}  // namespace xenon::launcher::frontend_backend
