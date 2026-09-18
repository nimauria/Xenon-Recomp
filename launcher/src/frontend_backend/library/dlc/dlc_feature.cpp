#include "dlc_feature.hpp"

#include "actions/dlc_action_catalog.hpp"

#include "../library_feature.hpp"
#include "../../settings/settings_feature.hpp"

namespace xenon::launcher::frontend_backend {

DlcFeature::DlcFeature(DlcService& dlc, LibraryFeature& library, SettingsFeature& settings,
                       bool test_mode, QObject* parent)
    : QObject(parent), dlc_(dlc), library_(library), settings_(settings), test_mode_(test_mode) {
  connect(&dlc_, &DlcService::changed, this, &DlcFeature::changed);
  connect(&settings_, &SettingsFeature::changed, this,
          [this](const QString& key, const QVariant&) {
            if (test_mode_ && key == QStringLiteral("developer/fixtureMode")) {
              removed_fixture_entries_.clear();
              emit changed({});
            } else if (key == QStringLiteral("library/missingContent")) {
              emit changed({});
            }
          });
}

QVariantList DlcFeature::entries(const QString& game_id) const {
  if (!test_mode_) return dlc_.entries(game_id);
  auto values = fixtureEntries(game_id);
  if (settings_.stringValue(QStringLiteral("library/missingContent"),
                            QStringLiteral("Show in catalogue")) ==
      QStringLiteral("Hide missing content")) {
    QVariantList installed;
    for (const auto& value : values) {
      if (value.toMap().value(QStringLiteral("installed")).toBool()) installed.append(value);
    }
    return installed;
  }
  return values;
}

QVariantMap DlcFeature::entry(const QString& game_id, const QString& dlc_id) const {
  for (const auto& value : entries(game_id)) {
    const auto item = value.toMap();
    if (item.value(QStringLiteral("dlcId")).toString() == dlc_id) return item;
  }
  return {};
}

QVariantList DlcFeature::actions(const QString& game_id, const QString& dlc_id) const {
  return DlcActionCatalog::actions(entry(game_id, dlc_id), test_mode_);
}

QVariantList DlcFeature::backgroundActions(const QString& game_id) const {
  return DlcActionCatalog::backgroundActions(!entries(game_id).isEmpty());
}

QVariantList DlcFeature::launchEntries(const QString& game_id) const {
  if (!test_mode_) return dlc_.launchEntries(game_id);

  QVariantList result;
  for (const auto& value : entries(game_id)) {
    const auto item = value.toMap();
    if (!item.value(QStringLiteral("installed")).toBool()) continue;
    QVariantMap mount;
    mount.insert(QStringLiteral("dlcId"), item.value(QStringLiteral("dlcId")));
    mount.insert(QStringLiteral("name"), item.value(QStringLiteral("name")));
    mount.insert(QStringLiteral("path"), item.value(QStringLiteral("path")));
    mount.insert(QStringLiteral("version"), item.value(QStringLiteral("version")));
    mount.insert(QStringLiteral("contentIds"), item.value(QStringLiteral("contentIds")));
    mount.insert(QStringLiteral("receipt"), item.value(QStringLiteral("receipt")));
    result.append(mount);
  }
  return result;
}

QString DlcFeature::rootPath(const QString& game_id) const {
  return test_mode_ ? QString{} : dlc_.ensureRootPath(game_id);
}

QString DlcFeature::itemPath(const QString& game_id, const QString& dlc_id) const {
  return test_mode_ ? QString{} : dlc_.itemPath(game_id, dlc_id);
}

ServiceResult DlcFeature::verify(const QString& game_id, const QString& dlc_id) const {
  if (!test_mode_) return dlc_.verify(game_id, dlc_id);
  const auto item = entry(game_id, dlc_id);
  if (item.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("DLC verification"),
                                  QStringLiteral("The fixture add-on no longer exists."));
  }
  if (!item.value(QStringLiteral("installed")).toBool()) {
    return ServiceResult::failure(QStringLiteral("DLC not installed"),
                                  QStringLiteral("The fixture add-on is intentionally marked missing."));
  }
  return ServiceResult::success(QStringLiteral("Fixture DLC verified"),
                                QStringLiteral("The launcher-side fixture state is internally consistent."));
}

ServiceResult DlcFeature::remove(const QString& game_id, const QString& dlc_id) {
  if (!test_mode_) return dlc_.remove(game_id, dlc_id);
  const auto item = entry(game_id, dlc_id);
  if (item.isEmpty() || !item.value(QStringLiteral("installed")).toBool()) {
    return ServiceResult::failure(QStringLiteral("Remove DLC"),
                                  QStringLiteral("The fixture add-on is not installed."));
  }
  removed_fixture_entries_.insert(game_id + QLatin1Char('/') + dlc_id);
  emit changed(game_id);
  return ServiceResult::success(QStringLiteral("Fixture DLC removed"),
                                QStringLiteral("The add-on is now marked missing for this test session."));
}

QVariantMap DlcFeature::fixtureEntry(QString game_id, QString dlc_id, QString name, bool installed,
                                     QString description) const {
  const auto key = game_id + QLatin1Char('/') + dlc_id;
  installed = installed && !removed_fixture_entries_.contains(key);
  QVariantMap item;
  item.insert(QStringLiteral("gameId"), game_id);
  item.insert(QStringLiteral("dlcId"), dlc_id);
  item.insert(QStringLiteral("name"), name);
  item.insert(QStringLiteral("description"), description);
  item.insert(QStringLiteral("installed"), installed);
  item.insert(QStringLiteral("state"), installed ? QStringLiteral("Installed")
                                                  : QStringLiteral("Not installed"));
  item.insert(QStringLiteral("path"), QString{});
  item.insert(QStringLiteral("folderName"), name);
  item.insert(QStringLiteral("managed"), true);
  item.insert(QStringLiteral("canVerify"), installed);
  item.insert(QStringLiteral("canRemove"), installed);
  item.insert(QStringLiteral("canOpen"), false);
  return item;
}

QString DlcFeature::fixtureMode() const {
  return settings_.stringValue(QStringLiteral("developer/fixtureMode"),
                               test_mode_ ? QStringLiteral("generic") : QStringLiteral("none"));
}

QVariantList DlcFeature::fixtureEntries(const QString& game_id) const {
  QVariantList result;
  const auto add = [this, &result, &game_id](const QString& id, const QString& name, bool installed,
                                             const QString& description = {}) {
    result.append(fixtureEntry(game_id, id, name, installed, description));
  };

  if (fixtureMode() == QStringLiteral("gracemeria") && game_id == QStringLiteral("4E4D07D1")) {
    add(QStringLiteral("cfa44-nosferatu"), QStringLiteral("CFA-44 Nosferatu"), true);
    add(QStringLiteral("ace-of-aces"), QStringLiteral("Ace of Aces Mission Pack"), true);
    add(QStringLiteral("garuda-skin"), QStringLiteral("F-15E Garuda Skin"), false);
    add(QStringLiteral("multiplayer-map-pack"), QStringLiteral("Multiplayer Map Pack"), true);
    add(QStringLiteral("extra-music-pack"), QStringLiteral("Extra Music Pack"), false);
    add(QStringLiteral("special-aircraft-set"), QStringLiteral("Special Aircraft Set"), true);
    add(QStringLiteral("additional-mission-set"), QStringLiteral("Additional Mission Set"), false);
    add(QStringLiteral("aircraft-skin-collection"), QStringLiteral("Aircraft Skin Collection"), true);
  } else if (game_id == QStringLiteral("TEST0001")) {
    add(QStringLiteral("expansion-alpha"), QStringLiteral("Test Expansion Alpha"), true);
    add(QStringLiteral("mission-pack"), QStringLiteral("Test Mission Pack"), true);
    add(QStringLiteral("vehicle-pack"), QStringLiteral("Test Vehicle Pack"), false);
    add(QStringLiteral("cosmetic-pack"), QStringLiteral("Test Cosmetic Pack"), false);
    add(QStringLiteral("challenge-pack"), QStringLiteral("Test Challenge Pack"), true);
    add(QStringLiteral("aircraft-pack"), QStringLiteral("Test Aircraft Pack"), true);
    add(QStringLiteral("music-pack"), QStringLiteral("Test Music Pack"), false);
    add(QStringLiteral("mission-beta"), QStringLiteral("Test Mission Beta"), true);
    add(QStringLiteral("livery-pack"), QStringLiteral("Test Livery Pack"), false);
    add(QStringLiteral("bonus-content"), QStringLiteral("Test Bonus Content"), true);
  } else if (game_id == QStringLiteral("TEST0002")) {
    add(QStringLiteral("arena-pack"), QStringLiteral("Test Arena Pack"), false);
    add(QStringLiteral("ruleset-pack"), QStringLiteral("Test Ruleset Pack"), true);
  }
  return result;
}

}  // namespace xenon::launcher::frontend_backend
