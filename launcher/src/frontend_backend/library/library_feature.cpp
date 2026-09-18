#include "library_feature.hpp"

#include "../settings/settings_feature.hpp"
#include "../modules/modules_feature.hpp"

#include <QFileInfo>
#include <utility>

namespace xenon::launcher::frontend_backend {

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

QVariantMap LibraryFeature::projected(QVariantMap item) const {
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
      auto item = value.toMap();
      const auto module_id = item.value(QStringLiteral("moduleId")).toString();
      const auto module = modules_.module(module_id);
      const auto module_installed = !module.isEmpty();
      const auto module_active = module_installed && module.value(QStringLiteral("active")).toBool();
      item.insert(QStringLiteral("moduleInstalled"), module_installed);
      item.insert(QStringLiteral("moduleActive"), module_active);
      item.insert(QStringLiteral("installed"), module_installed);
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
