#include "modules_feature.hpp"

#include "../settings/settings_feature.hpp"
#include "../../services/path_service.hpp"

#include <QFileInfo>
#include <QStringList>
#include <QTimer>
#include <utility>

namespace xenon::launcher::frontend_backend {
namespace {
QVariantMap moduleItem(const QString& name, const QString& type, const QString& version,
                       const QString& status, bool active, bool update_available,
                       const QString& description, const QString& id, const QString& regions,
                       const QString& game_ids, const QString& runtime, const QString& renderer,
                       const QString& capabilities, const QString& linked_game) {
  QVariantMap item;
  item.insert(QStringLiteral("moduleName"), name);
  item.insert(QStringLiteral("moduleType"), type);
  item.insert(QStringLiteral("version"), version);
  item.insert(QStringLiteral("status"), status);
  item.insert(QStringLiteral("active"), active);
  item.insert(QStringLiteral("updateAvailable"), update_available);
  item.insert(QStringLiteral("description"), description);
  item.insert(QStringLiteral("moduleId"), id);
  item.insert(QStringLiteral("regions"), regions);
  item.insert(QStringLiteral("gameIds"), game_ids);
  item.insert(QStringLiteral("runtimeDependency"), runtime);
  item.insert(QStringLiteral("renderer"), renderer);
  item.insert(QStringLiteral("artwork"), QString{});
  item.insert(QStringLiteral("capabilities"), capabilities);
  item.insert(QStringLiteral("linkedGame"), linked_game);
  item.insert(QStringLiteral("path"), QString{});
  return item;
}

QVariantMap setting(QString id, QString label, QString description, QString type,
                    QVariant default_value, QVariantList options = {}) {
  QVariantMap item;
  item.insert(QStringLiteral("id"), std::move(id));
  item.insert(QStringLiteral("label"), std::move(label));
  item.insert(QStringLiteral("description"), std::move(description));
  item.insert(QStringLiteral("type"), std::move(type));
  item.insert(QStringLiteral("defaultValue"), std::move(default_value));
  if (!options.isEmpty()) item.insert(QStringLiteral("options"), std::move(options));
  return item;
}
}  // namespace

ModulesFeature::ModulesFeature(ModuleService& modules, LibraryService& library,
                               PackageService& packages, PathService& paths, SettingsFeature& settings,
                               bool test_mode, bool suppress_startup_external_work, QObject* parent)
    : QObject(parent),
      modules_(modules),
      library_(library),
      settings_(settings),
      test_mode_(test_mode),
      suppress_startup_external_work_(suppress_startup_external_work),
      catalog_(nullptr),
      asset_cache_(paths, nullptr),
      importer_(modules, nullptr),
      updater_(packages, modules, nullptr) {
  connect(&modules_, &ModuleService::changed, this, [this]() {
    if (!test_mode_) emit changed();
  });
  connect(&settings_, &SettingsFeature::changed, this,
          [this](const QString& key, const QVariant&) {
            if (test_mode_ && key == QStringLiteral("developer/fixtureMode")) {
              rebuildFixtures();
              emit changed();
            }
            if (key == QStringLiteral("updates/modules") ||
                key == QStringLiteral("updates/modulePrerelease")) {
              emit catalogChanged();
            }
          });
  connect(&catalog_, &GitHubModuleCatalogProvider::changed, this, [this]() {
    emit catalogChanged();
  });
  connect(&catalog_, &GitHubModuleCatalogProvider::refreshed, this, [this]() {
    syncCatalogAssets();
    emit catalogChanged();
    emit changed();
    if (settings_.boolValue(QStringLiteral("updates/modules"), true)) {
      (void)checkAllUpdates();
    }
  });
  connect(&asset_cache_, &ModuleCatalogAssetCache::changed, this,
          [this](const QString&) {
            emit catalogChanged();
            emit changed();
          });
  connect(&updater_, &ModuleUpdateService::changed, this,
          [this](const QString& module_id) {
            emit updateStateChanged(module_id);
            emit changed();
            emit catalogChanged();
            if (install_requests_.contains(module_id)) {
              QTimer::singleShot(0, this, [this, module_id]() {
                continueInstallRequest(module_id);
              });
            }
          });
  connect(&updater_, &ModuleUpdateService::historyChanged, this,
          [this](const QString& module_id) {
            emit updateHistoryChanged(module_id);
            emit changed();
          });
  connect(&updater_, &ModuleUpdateService::moduleInstalled, this,
          [this](const QString& module_id) {
            install_requests_.remove(module_id);
            (void)modules_.refresh();
            emit changed();
            emit catalogChanged();
          });
  connect(&updater_, &ModuleUpdateService::notificationRequested, this,
          &ModulesFeature::notificationRequested);

  if (test_mode_) rebuildFixtures();
  if (!suppress_startup_external_work_) {
    syncCatalogAssets();
    catalog_.refresh();
  }
}

QString ModulesFeature::fixtureMode() const {
  return settings_.stringValue(QStringLiteral("developer/fixtureMode"),
                               test_mode_ ? QStringLiteral("generic") : QStringLiteral("none"));
}

void ModulesFeature::rebuildFixtures() {
  fixture_modules_.clear();
  fixture_update_states_.clear();
  const auto mode = fixtureMode();
  if (mode == QStringLiteral("none")) return;
  if (mode == QStringLiteral("gracemeria")) {
    fixture_modules_.append(moduleItem(
        QStringLiteral("Project Gracemeria"), QStringLiteral("Game Module"), QStringLiteral("preview"),
        QStringLiteral("Active"), true, false,
        QStringLiteral("Project Gracemeria launcher preview. No commercial game content is bundled; users provide their own local files."),
        QStringLiteral("org.nimauria.project-gracemeria"), QStringLiteral("NTSC-U / PAL"),
        QStringLiteral("Module declared"), QStringLiteral("Xenon Recomp"),
        QStringLiteral("Vulkan / Direct3D 12"),
        QStringLiteral("Game identification • DLC catalogue • Saves • Offline services • Module settings"),
        QStringLiteral("Ace Combat 6: Fires of Liberation")));
    return;
  }

  fixture_modules_.append(moduleItem(
      QStringLiteral("Test Flight Module"), QStringLiteral("Game Module"), QStringLiteral("0.1-test"),
      QStringLiteral("Active"), true, false,
      QStringLiteral("Fictional game module used to exercise discovery, compatibility, update and module settings UI."),
      QStringLiteral("xenon.test.flight"), QStringLiteral("Test Region A / B"), QStringLiteral("TEST0001"),
      QStringLiteral("Xenon Recomp"), QStringLiteral("Vulkan"),
      QStringLiteral("Game identification • DLC catalogue • Save data • Offline launch"),
      QStringLiteral("Xenon Test Flight")));
  fixture_modules_.append(moduleItem(
      QStringLiteral("Test Arena Module"), QStringLiteral("Game Module"), QStringLiteral("0.2-test"),
      QStringLiteral("Disabled"), false, true,
      QStringLiteral("Second fictional module used to verify disabled states and update indicators."),
      QStringLiteral("xenon.test.arena"), QStringLiteral("Module-defined"), QStringLiteral("TEST0002"),
      QStringLiteral("Xenon Recomp"), QStringLiteral("Automatic"),
      QStringLiteral("Game identification • Local content validation"), QString{}));
  fixture_modules_.append(moduleItem(
      QStringLiteral("Test Compatibility Pack"), QStringLiteral("Support Package"), QStringLiteral("0.1-test"),
      QStringLiteral("Installed"), true, false,
      QStringLiteral("Fictional shared support package used to exercise non-game module presentation."),
      QStringLiteral("xenon.test.compat"), QStringLiteral("Global"), QStringLiteral("Multiple"),
      QStringLiteral("Xenon Recomp"), QStringLiteral("N/A"),
      QStringLiteral("Compatibility metadata • Validation definitions"), QString{}));

  QVariantMap update;
  update.insert(QStringLiteral("moduleId"), QStringLiteral("xenon.test.arena"));
  update.insert(QStringLiteral("status"), QStringLiteral("update-available"));
  update.insert(QStringLiteral("statusMessage"), QStringLiteral("Fixture update 0.3-test is available."));
  update.insert(QStringLiteral("installedVersion"), QStringLiteral("0.2-test"));
  update.insert(QStringLiteral("availableVersion"), QStringLiteral("0.3-test"));
  update.insert(QStringLiteral("canDownload"), true);
  update.insert(QStringLiteral("canInstall"), false);
  update.insert(QStringLiteral("updateAvailable"), true);
  update.insert(QStringLiteral("downloadProgress"), 0.0);
  fixture_update_states_.insert(QStringLiteral("xenon.test.arena"), update);
}

QVariantMap ModulesFeature::catalogEntry(const QString& module_id) const {
  for (const auto& value : catalog_.entries()) {
    const auto item = value.toMap();
    if (item.value(QStringLiteral("moduleId")).toString() == module_id) return item;
  }
  return {};
}

QVariantMap ModulesFeature::catalogEntryData(const QString& module_id) const {
  auto item = catalogEntry(module_id);
  if (item.isEmpty()) return item;
  auto launcher = item.value(QStringLiteral("launcher")).toMap();
  const auto tile = asset_cache_.localAssetUrl(module_id, QStringLiteral("tileArt"));
  const auto hero = asset_cache_.localAssetUrl(module_id, QStringLiteral("heroArt"));
  if (!tile.isEmpty()) launcher.insert(QStringLiteral("tileArtLocalUrl"), tile);
  if (!hero.isEmpty()) launcher.insert(QStringLiteral("heroArtLocalUrl"), hero);
  launcher.insert(QStringLiteral("assetCacheState"), asset_cache_.state(module_id));
  item.insert(QStringLiteral("launcher"), launcher);
  return item;
}

QVariantMap ModulesFeature::launcherMetadata(const QString& module_id) const {
  return catalogEntryData(module_id).value(QStringLiteral("launcher")).toMap();
}

QVariantList ModulesFeature::catalogDlc(const QString& module_id) const {
  return catalogEntryData(module_id).value(QStringLiteral("dlc")).toList();
}

ServiceResult ModulesFeature::refreshPresentationMetadata(const QString& module_id) {
  if (catalogEntry(module_id).isEmpty()) {
    return ServiceResult::failure(
        QStringLiteral("Game metadata refresh"),
        QStringLiteral("This game module is not present in the official Xenon Modules registry."));
  }
  catalog_.refresh();
  return ServiceResult::success(
      QStringLiteral("Game metadata refresh started"),
      QStringLiteral("Xenon is refreshing the official module registry and presentation assets from GitHub."));
}

void ModulesFeature::syncCatalogAssets() {
  asset_cache_.sync(catalog_.entries());
}

QVariantMap ModulesFeature::fixtureUpdateState(const QString& module_id) const {
  auto value = fixture_update_states_.value(module_id);
  if (value.isEmpty()) {
    value.insert(QStringLiteral("moduleId"), module_id);
    value.insert(QStringLiteral("status"), QStringLiteral("idle"));
    value.insert(QStringLiteral("statusMessage"), QStringLiteral("Updates have not been checked yet."));
    value.insert(QStringLiteral("canDownload"), false);
    value.insert(QStringLiteral("canInstall"), false);
    value.insert(QStringLiteral("updateAvailable"), false);
    value.insert(QStringLiteral("downloadProgress"), 0.0);
  }
  return value;
}

QVariantMap ModulesFeature::decorated(QVariantMap item) const {
  if (item.isEmpty()) return item;
  const auto module_id = item.value(QStringLiteral("moduleId")).toString();
  const auto catalog_item = catalogEntry(module_id);
  if (!catalog_item.isEmpty()) {
    item.insert(QStringLiteral("catalogKnown"), true);
    item.insert(QStringLiteral("repository"), catalog_item.value(QStringLiteral("repository")));
    item.insert(QStringLiteral("repositoryUrl"), catalog_item.value(QStringLiteral("repositoryUrl")));
    item.insert(QStringLiteral("registryEntryUrl"), catalog_item.value(QStringLiteral("registryEntryUrl")));
    item.insert(QStringLiteral("publisher"), catalog_item.value(QStringLiteral("publisher")));
    item.insert(QStringLiteral("license"), catalog_item.value(QStringLiteral("license")));
    item.insert(QStringLiteral("catalogVerified"), catalog_item.value(QStringLiteral("verified")));
    item.insert(QStringLiteral("packageSupported"), catalog_item.value(QStringLiteral("packageSupported")));
  } else {
    item.insert(QStringLiteral("catalogKnown"), false);
    item.insert(QStringLiteral("catalogVerified"), false);
    item.insert(QStringLiteral("packageSupported"), false);
  }

  QStringList linked_titles;
  if (!test_mode_) {
    for (const auto& value : library_.entries()) {
      const auto game = value.toMap();
      if (game.value(QStringLiteral("moduleId")).toString() == module_id) {
        const auto title = game.value(QStringLiteral("title")).toString().trimmed();
        if (!title.isEmpty()) linked_titles.append(title);
      }
    }
  } else {
    const auto linked = item.value(QStringLiteral("linkedGame")).toString().trimmed();
    if (!linked.isEmpty()) linked_titles.append(linked);
  }
  item.insert(QStringLiteral("linkedGameCount"), linked_titles.size());
  item.insert(QStringLiteral("linkedGames"), linked_titles.join(QStringLiteral(" • ")));
  item.insert(QStringLiteral("linkedGame"), linked_titles.isEmpty()
                                               ? QString{}
                                               : linked_titles.size() == 1
                                                     ? linked_titles.first()
                                                     : QStringLiteral("%1 library games").arg(linked_titles.size()));
  item.insert(QStringLiteral("settingsCount"), settingsSchema(module_id).size());
  const auto local_dlc_count = test_mode_ ? 0 : modules_.dlcCatalog(module_id).size();
  const auto registry_dlc_count = catalogDlc(module_id).size();
  item.insert(QStringLiteral("dlcDefinitionCount"),
              qMax(local_dlc_count, registry_dlc_count));

  const auto update = updateState(module_id);
  item.insert(QStringLiteral("updateStatus"), update.value(QStringLiteral("status")));
  item.insert(QStringLiteral("updateMessage"), update.value(QStringLiteral("statusMessage")));
  item.insert(QStringLiteral("availableVersion"), update.value(QStringLiteral("availableVersion")));
  item.insert(QStringLiteral("updateAvailable"), update.value(QStringLiteral("updateAvailable"),
                                                                  item.value(QStringLiteral("updateAvailable"))).toBool());
  item.insert(QStringLiteral("canDownloadUpdate"), update.value(QStringLiteral("canDownload")));
  item.insert(QStringLiteral("canInstallUpdate"), update.value(QStringLiteral("canInstall")));
  item.insert(QStringLiteral("downloadProgress"), update.value(QStringLiteral("downloadProgress"), 0.0));
  item.insert(QStringLiteral("downloadedBytes"), update.value(QStringLiteral("downloadedBytes"), 0));
  item.insert(QStringLiteral("downloadTotalBytes"), update.value(QStringLiteral("downloadTotalBytes"), 0));
  item.insert(QStringLiteral("stagedAt"), update.value(QStringLiteral("stagedAt")));
  item.insert(QStringLiteral("verifiedDigest"), update.value(QStringLiteral("verifiedDigest")));
  item.insert(QStringLiteral("releaseUrl"), update.value(QStringLiteral("releaseUrl")));
  item.insert(QStringLiteral("releaseName"), update.value(QStringLiteral("releaseName")));
  item.insert(QStringLiteral("releaseNotes"), update.value(QStringLiteral("releaseNotes")));
  item.insert(QStringLiteral("publishedAt"), update.value(QStringLiteral("publishedAt")));
  item.insert(QStringLiteral("assetName"), update.value(QStringLiteral("assetName"),
                                                        catalog_item.value(QStringLiteral("assetName"))));
  item.insert(QStringLiteral("assetSize"), update.value(QStringLiteral("assetSize"), 0));
  item.insert(QStringLiteral("rollbackAvailable"), update.value(QStringLiteral("rollbackAvailable"), false));
  item.insert(QStringLiteral("rollbackVersion"), update.value(QStringLiteral("rollbackVersion")));
  item.insert(QStringLiteral("rollbackCreatedAt"), update.value(QStringLiteral("rollbackCreatedAt")));

  if (!item.value(QStringLiteral("active")).toBool() && !linked_titles.isEmpty()) {
    item.insert(QStringLiteral("impactMessage"),
                linked_titles.size() == 1
                    ? QStringLiteral("1 library game is disabled until this module is enabled.")
                    : QStringLiteral("%1 library games are disabled until this module is enabled.")
                          .arg(linked_titles.size()));
  } else {
    item.insert(QStringLiteral("impactMessage"), QString{});
  }
  return item;
}

QVariantList ModulesFeature::entries() const {
  QVariantList result;
  const auto source = test_mode_ ? fixture_modules_ : modules_.modules();
  for (const auto& value : source) result.append(decorated(value.toMap()));
  return result;
}

QVariantMap ModulesFeature::module(const QString& module_id) const {
  if (!test_mode_) return decorated(modules_.module(module_id));
  for (const auto& value : fixture_modules_) {
    const auto item = value.toMap();
    if (item.value(QStringLiteral("moduleId")).toString() == module_id) return decorated(item);
  }
  return {};
}

QVariantList ModulesFeature::actions(const QString& module_id) const {
  return ModuleActionCatalog::actions(module(module_id), updateState(module_id),
                                      !settingsSchema(module_id).isEmpty());
}

QVariantList ModulesFeature::pageActions() const { return ModuleActionCatalog::pageActions(); }

ServiceResult ModulesFeature::refresh() {
  if (!test_mode_) return modules_.refresh();
  rebuildFixtures();
  emit changed();
  return ServiceResult::success(QStringLiteral("Modules refreshed"),
                                QStringLiteral("Test module fixtures were rebuilt by the launcher core."));
}

ServiceResult ModulesFeature::importPackages(const QList<QUrl>& sources) {
  if (!test_mode_) return importer_.importSources(sources);
  if (sources.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module import"), QStringLiteral("No module package was selected."));
  }
  return ServiceResult::success(QStringLiteral("Module package selected"),
                                QStringLiteral("Test mode does not modify real module installations."));
}

ServiceResult ModulesFeature::setEnabled(const QString& module_id, bool enabled) {
  if (!test_mode_) return modules_.setEnabled(module_id, enabled);
  for (qsizetype i = 0; i < fixture_modules_.size(); ++i) {
    auto item = fixture_modules_.at(i).toMap();
    if (item.value(QStringLiteral("moduleId")).toString() != module_id) continue;
    item.insert(QStringLiteral("active"), enabled);
    item.insert(QStringLiteral("status"), enabled ? QStringLiteral("Active") : QStringLiteral("Disabled"));
    fixture_modules_[i] = item;
    emit changed();
    return ServiceResult::success(enabled ? QStringLiteral("Module enabled") : QStringLiteral("Module disabled"),
                                  QStringLiteral("%1 is now %2 in this test session.")
                                      .arg(item.value(QStringLiteral("moduleName")).toString(),
                                           enabled ? QStringLiteral("enabled") : QStringLiteral("disabled")));
  }
  return ServiceResult::failure(QStringLiteral("Module state"), QStringLiteral("The selected module no longer exists."));
}

ServiceResult ModulesFeature::remove(const QString& module_id) {
  if (!test_mode_) return modules_.remove(module_id);
  for (qsizetype i = 0; i < fixture_modules_.size(); ++i) {
    if (fixture_modules_.at(i).toMap().value(QStringLiteral("moduleId")).toString() != module_id) continue;
    fixture_modules_.removeAt(i);
    fixture_update_states_.remove(module_id);
    emit changed();
    emit catalogChanged();
    return ServiceResult::success(QStringLiteral("Fixture module removed"),
                                  QStringLiteral("The module was removed from this test session."));
  }
  return ServiceResult::failure(QStringLiteral("Module removal"), QStringLiteral("The selected module no longer exists."));
}

ServiceResult ModulesFeature::verify(const QString& module_id) const {
  if (!test_mode_) return modules_.verify(module_id);
  if (module(module_id).isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module verification"), QStringLiteral("The selected fixture module no longer exists."));
  }
  return ServiceResult::success(QStringLiteral("Module verified"),
                                QStringLiteral("The launcher-side fixture manifest is internally consistent."));
}

QString ModulesFeature::modulePath(const QString& module_id) const {
  return test_mode_ ? module(module_id).value(QStringLiteral("path")).toString() : modules_.modulePath(module_id);
}

QVariantList ModulesFeature::genericSettings(const QString& module_id) const {
  QVariantList schema;
  schema.append(setting(QStringLiteral("renderer"), QStringLiteral("Renderer override"),
                        QStringLiteral("Use the launcher default or select a module-supported renderer."),
                        QStringLiteral("choice"), QStringLiteral("Launcher default"),
                        {QStringLiteral("Launcher default"), QStringLiteral("Vulkan"), QStringLiteral("Direct3D 12")}));
  schema.append(setting(QStringLiteral("offline"), QStringLiteral("Offline mode"),
                        QStringLiteral("Force offline-compatible services for this module."),
                        QStringLiteral("bool"), true));
  schema.append(setting(QStringLiteral("debugOverlay"), QStringLiteral("Debug overlay"),
                        QStringLiteral("Show module diagnostics while launching test content."),
                        QStringLiteral("bool"), false));
  return applyFixtureValues(module_id, schema);
}

QVariantList ModulesFeature::gracemeriaSettings(const QString& module_id) const {
  QVariantList schema;
  schema.append(setting(QStringLiteral("region"), QStringLiteral("Game region"),
                        QStringLiteral("Choose which supported executable/content region the module should prefer."),
                        QStringLiteral("choice"), QStringLiteral("Automatic"),
                        {QStringLiteral("Automatic"), QStringLiteral("NTSC-U"), QStringLiteral("PAL")}));
  schema.append(setting(QStringLiteral("renderer"), QStringLiteral("Renderer override"),
                        QStringLiteral("Override the launcher renderer for this module."),
                        QStringLiteral("choice"), QStringLiteral("Launcher default"),
                        {QStringLiteral("Launcher default"), QStringLiteral("Vulkan"), QStringLiteral("Direct3D 12")}));
  schema.append(setting(QStringLiteral("offline"), QStringLiteral("Offline mode"),
                        QStringLiteral("Use local service fallbacks while online services are unavailable."),
                        QStringLiteral("bool"), true));
  schema.append(setting(QStringLiteral("skipIntro"), QStringLiteral("Skip intro videos"),
                        QStringLiteral("Example module-defined preference used only by the UI preview."),
                        QStringLiteral("bool"), false));
  schema.append(setting(QStringLiteral("cache"), QStringLiteral("Shader cache profile"),
                        QStringLiteral("Example scalable manifest choice."), QStringLiteral("choice"),
                        QStringLiteral("Automatic"),
                        {QStringLiteral("Automatic"), QStringLiteral("Conservative"), QStringLiteral("Aggressive")}));
  return applyFixtureValues(module_id, schema);
}

QVariantList ModulesFeature::applyFixtureValues(const QString& module_id, QVariantList schema) const {
  const auto values = fixture_settings_.value(module_id);
  for (qsizetype i = 0; i < schema.size(); ++i) {
    auto definition = schema.at(i).toMap();
    const auto id = definition.value(QStringLiteral("id")).toString();
    if (values.contains(id)) definition.insert(QStringLiteral("defaultValue"), values.value(id));
    schema[i] = definition;
  }
  return schema;
}

QVariantList ModulesFeature::settingsSchema(const QString& module_id) const {
  if (!test_mode_) return modules_.settingsSchema(module_id);
  if (module_id == QStringLiteral("org.nimauria.project-gracemeria")) return gracemeriaSettings(module_id);
  if (module_id.startsWith(QStringLiteral("xenon.test."))) return genericSettings(module_id);
  return {};
}

QVariantMap ModulesFeature::settingsValues(const QString& module_id) const {
  if (!test_mode_) return modules_.settingsValues(module_id);
  QVariantMap values;
  for (const auto& candidate : settingsSchema(module_id)) {
    const auto definition = candidate.toMap();
    const auto id = definition.value(QStringLiteral("id")).toString();
    if (!id.isEmpty()) values.insert(id, definition.value(QStringLiteral("defaultValue")));
  }
  return values;
}

ServiceResult ModulesFeature::setSetting(const QString& module_id, const QString& setting_id,
                                         const QVariant& value) {
  if (!test_mode_) return modules_.setSetting(module_id, setting_id, value);
  QVariantMap definition;
  for (const auto& candidate : settingsSchema(module_id)) {
    const auto item = candidate.toMap();
    if (item.value(QStringLiteral("id")).toString() == setting_id) {
      definition = item;
      break;
    }
  }
  if (definition.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module setting"), QStringLiteral("The module does not declare this setting."));
  }
  auto values = fixture_settings_.value(module_id);
  values.insert(setting_id, value);
  fixture_settings_.insert(module_id, values);
  emit changed();
  return ServiceResult::success(QStringLiteral("Module setting saved"),
                                QStringLiteral("%1 was updated in this test session.")
                                    .arg(definition.value(QStringLiteral("label"), setting_id).toString()));
}

QString ModulesFeature::installedVersion(const QString& module_id) const {
  if (!test_mode_) return modules_.module(module_id).value(QStringLiteral("version")).toString();
  for (const auto& value : fixture_modules_) {
    const auto item = value.toMap();
    if (item.value(QStringLiteral("moduleId")).toString() == module_id)
      return item.value(QStringLiteral("version")).toString();
  }
  return {};
}

QVariantMap ModulesFeature::updateState(const QString& module_id) const {
  if (test_mode_) {
    for (const auto& value : fixture_modules_) {
      if (value.toMap().value(QStringLiteral("moduleId")).toString() == module_id) {
        return fixtureUpdateState(module_id);
      }
    }
  }
  return updater_.state(module_id);
}

QVariantList ModulesFeature::updateHistory(const QString& module_id) const {
  if (test_mode_) return {};
  return updater_.history(module_id);
}

QVariantList ModulesFeature::catalogEntries() const {
  QVariantList result;
  for (const auto& value : catalog_.entries()) {
    auto item = catalogEntryData(value.toMap().value(QStringLiteral("moduleId")).toString());
    const auto id = item.value(QStringLiteral("moduleId")).toString();
    const auto installed = !installedVersion(id).isEmpty();
    const auto installed_module = module(id);
    const auto update = test_mode_ && installed ? fixtureUpdateState(id) : updater_.state(id);
    item.insert(QStringLiteral("installed"), installed);
    item.insert(QStringLiteral("installedVersion"), installedVersion(id));
    item.insert(QStringLiteral("active"), installed_module.value(QStringLiteral("active")).toBool());
    item.insert(QStringLiteral("updateStatus"), update.value(QStringLiteral("status")));
    item.insert(QStringLiteral("updateMessage"), update.value(QStringLiteral("statusMessage")));
    item.insert(QStringLiteral("availableVersion"), update.value(QStringLiteral("availableVersion")));
    item.insert(QStringLiteral("updateAvailable"), update.value(QStringLiteral("updateAvailable")));
    item.insert(QStringLiteral("canDownload"), update.value(QStringLiteral("canDownload")));
    item.insert(QStringLiteral("canInstall"), update.value(QStringLiteral("canInstall")));
    item.insert(QStringLiteral("downloadProgress"), update.value(QStringLiteral("downloadProgress"), 0.0));
    item.insert(QStringLiteral("downloadedBytes"), update.value(QStringLiteral("downloadedBytes"), 0));
    item.insert(QStringLiteral("downloadTotalBytes"), update.value(QStringLiteral("downloadTotalBytes"), 0));
    item.insert(QStringLiteral("stagedAt"), update.value(QStringLiteral("stagedAt")));
    item.insert(QStringLiteral("verifiedDigest"), update.value(QStringLiteral("verifiedDigest")));
    item.insert(QStringLiteral("releaseUrl"), update.value(QStringLiteral("releaseUrl")));
    item.insert(QStringLiteral("releaseName"), update.value(QStringLiteral("releaseName")));
    item.insert(QStringLiteral("releaseNotes"), update.value(QStringLiteral("releaseNotes")));
    item.insert(QStringLiteral("publishedAt"), update.value(QStringLiteral("publishedAt")));
    item.insert(QStringLiteral("assetSize"), update.value(QStringLiteral("assetSize"), 0));
    item.insert(QStringLiteral("rollbackAvailable"), update.value(QStringLiteral("rollbackAvailable"), false));
    item.insert(QStringLiteral("rollbackVersion"), update.value(QStringLiteral("rollbackVersion")));
    item.insert(QStringLiteral("installable"), item.value(QStringLiteral("packageSupported")).toBool());
    result.append(item);
  }
  return result;
}

QVariantMap ModulesFeature::catalogState() const { return catalog_.state(); }

ServiceResult ModulesFeature::refreshCatalog() {
  catalog_.refresh();
  return ServiceResult::success(QStringLiteral("Module catalog refresh started"),
                                QStringLiteral("Xenon is refreshing the official Xenon Modules registry from GitHub."));
}

ServiceResult ModulesFeature::checkForUpdate(const QString& module_id) {
  const auto current_update = updateState(module_id);
  const auto current_status = current_update.value(QStringLiteral("status")).toString();
  if (current_status == QStringLiteral("checking") ||
      current_status == QStringLiteral("downloading") ||
      current_status == QStringLiteral("installing") ||
      current_status == QStringLiteral("rolling-back")) {
    return ServiceResult::failure(QStringLiteral("Module update"),
                                  QStringLiteral("An update operation is already in progress for this module."));
  }
  if (current_update.value(QStringLiteral("canInstall")).toBool()) {
    return ServiceResult::failure(QStringLiteral("Module update"),
                                  QStringLiteral("Install the already verified staged package before checking this module again."));
  }

  const auto entry = catalogEntry(module_id);
  if (entry.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module update"),
                                  QStringLiteral("This module is not present in the official Xenon Modules registry, so Xenon has no trusted GitHub release source for it."));
  }

  if (test_mode_ && !module(module_id).isEmpty()) {
    auto state = fixtureUpdateState(module_id);
    if (state.value(QStringLiteral("status")).toString() == QStringLiteral("idle")) {
      state.insert(QStringLiteral("status"), QStringLiteral("up-to-date"));
      state.insert(QStringLiteral("statusMessage"), QStringLiteral("The fixture module is current for this test session."));
      state.insert(QStringLiteral("installedVersion"), installedVersion(module_id));
      fixture_update_states_.insert(module_id, state);
    }
    emit updateStateChanged(module_id);
    emit changed();
    emit catalogChanged();
    return ServiceResult::success(QStringLiteral("Fixture update check complete"),
                                  state.value(QStringLiteral("statusMessage")).toString());
  }

  updater_.check(module_id, entry, installedVersion(module_id),
                 settings_.boolValue(QStringLiteral("updates/modulePrerelease"), false));
  return ServiceResult::success(QStringLiteral("Module update check started"),
                                QStringLiteral("Xenon is checking the module's GitHub releases."));
}

ServiceResult ModulesFeature::checkAllUpdates() {
  int started = 0;
  for (const auto& value : entries()) {
    const auto item = value.toMap();
    const auto id = item.value(QStringLiteral("moduleId")).toString();
    if (catalogEntry(id).isEmpty()) continue;
    const auto result = checkForUpdate(id);
    if (result.ok) ++started;
  }
  if (started == 0) {
    return ServiceResult::success(QStringLiteral("Module update check"),
                                  QStringLiteral("No installed modules currently have an official Xenon Modules registry update source."));
  }
  return ServiceResult::success(QStringLiteral("Module update checks started"),
                                QStringLiteral("Checking %1 installed module(s) against their GitHub releases.").arg(started));
}

ServiceResult ModulesFeature::downloadUpdate(const QString& module_id) {
  if (test_mode_ && !module(module_id).isEmpty()) {
    auto state = fixtureUpdateState(module_id);
    if (!state.value(QStringLiteral("canDownload")).toBool()) {
      return ServiceResult::failure(QStringLiteral("Fixture module update"),
                                    QStringLiteral("This fixture does not have an update ready to download."));
    }
    state.insert(QStringLiteral("status"), QStringLiteral("ready-to-install"));
    state.insert(QStringLiteral("statusMessage"), QStringLiteral("Fixture package downloaded and verified."));
    state.insert(QStringLiteral("canDownload"), false);
    state.insert(QStringLiteral("canInstall"), true);
    state.insert(QStringLiteral("downloadProgress"), 1.0);
    fixture_update_states_.insert(module_id, state);
    emit updateStateChanged(module_id);
    emit changed();
    emit catalogChanged();
    return ServiceResult::success(QStringLiteral("Fixture package ready"));
  }
  return updater_.download(module_id);
}

ServiceResult ModulesFeature::cancelUpdateDownload(const QString& module_id) {
  if (test_mode_) {
    return ServiceResult::failure(QStringLiteral("Fixture module update"),
                                  QStringLiteral("Fixture downloads complete immediately."));
  }
  return updater_.cancelDownload(module_id);
}

ServiceResult ModulesFeature::installUpdate(const QString& module_id) {
  if (test_mode_ && !module(module_id).isEmpty()) {
    auto state = fixtureUpdateState(module_id);
    if (!state.value(QStringLiteral("canInstall")).toBool()) {
      return ServiceResult::failure(QStringLiteral("Fixture module update"),
                                    QStringLiteral("Download the fixture update first."));
    }
    const auto version = state.value(QStringLiteral("availableVersion"), QStringLiteral("0.3-test")).toString();
    for (qsizetype i = 0; i < fixture_modules_.size(); ++i) {
      auto item = fixture_modules_.at(i).toMap();
      if (item.value(QStringLiteral("moduleId")).toString() != module_id) continue;
      item.insert(QStringLiteral("version"), version);
      item.insert(QStringLiteral("updateAvailable"), false);
      fixture_modules_[i] = item;
      break;
    }
    state.insert(QStringLiteral("status"), QStringLiteral("up-to-date"));
    state.insert(QStringLiteral("statusMessage"), QStringLiteral("Fixture module updated successfully."));
    state.insert(QStringLiteral("installedVersion"), version);
    state.insert(QStringLiteral("canDownload"), false);
    state.insert(QStringLiteral("canInstall"), false);
    state.insert(QStringLiteral("updateAvailable"), false);
    fixture_update_states_.insert(module_id, state);
    emit updateStateChanged(module_id);
    emit changed();
    emit catalogChanged();
    return ServiceResult::success(QStringLiteral("Fixture module updated"),
                                  QStringLiteral("The fixture update path completed successfully."));
  }
  return updater_.install(module_id);
}

ServiceResult ModulesFeature::rollbackUpdate(const QString& module_id) {
  if (test_mode_) {
    return ServiceResult::failure(QStringLiteral("Fixture module rollback"),
                                  QStringLiteral("Fixture modules do not create persistent rollback snapshots."));
  }
  return updater_.rollback(module_id);
}

ServiceResult ModulesFeature::clearUpdateHistory(const QString& module_id) {
  if (test_mode_) {
    return ServiceResult::success(QStringLiteral("Fixture update history"),
                                  QStringLiteral("Fixture mode does not persist module update history."));
  }
  return updater_.clearHistory(module_id);
}

ServiceResult ModulesFeature::requestInstall(const QString& module_id) {
  if (!installedVersion(module_id).isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("This module is already installed. Use the module updater for newer releases."));
  }
  if (catalogEntry(module_id).isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("This module is not present in the official Xenon Modules registry."));
  }

  install_requests_.insert(module_id);
  QTimer::singleShot(0, this, [this, module_id]() { continueInstallRequest(module_id); });
  return ServiceResult::success(
      QStringLiteral("Module install started"),
      QStringLiteral("Xenon will check the module release, download and verify the package, then install it automatically."));
}

void ModulesFeature::continueInstallRequest(const QString& module_id) {
  if (!install_requests_.contains(module_id)) return;

  if (!installedVersion(module_id).isEmpty()) {
    install_requests_.remove(module_id);
    return;
  }

  const auto state = updater_.state(module_id);
  const auto status = state.value(QStringLiteral("status")).toString();

  if (status == QStringLiteral("error") ||
      status == QStringLiteral("no-release") ||
      status == QStringLiteral("package-unavailable") ||
      status == QStringLiteral("cancelled")) {
    install_requests_.remove(module_id);
    return;
  }

  if (status == QStringLiteral("checking") ||
      status == QStringLiteral("downloading") ||
      status == QStringLiteral("installing") ||
      status == QStringLiteral("rolling-back")) {
    return;
  }

  ServiceResult result;
  if (state.value(QStringLiteral("canInstall")).toBool()) {
    result = installUpdate(module_id);
  } else if (state.value(QStringLiteral("canDownload")).toBool()) {
    result = downloadUpdate(module_id);
  } else {
    result = checkForUpdate(module_id);
  }

  if (!result.ok) install_requests_.remove(module_id);
}

ServiceResult ModulesFeature::requestUpdate(const QString& module_id) {
  return checkForUpdate(module_id);
}

ServiceResult ModulesFeature::unlinkGame(const QString& module_id) {
  if (!test_mode_) {
    return ServiceResult::failure(QStringLiteral("Module game link"),
                                  QStringLiteral("Game/module associations are owned by content identification. Remove a game from Library rather than unlinking a detected module."));
  }
  for (qsizetype i = 0; i < fixture_modules_.size(); ++i) {
    auto item = fixture_modules_.at(i).toMap();
    if (item.value(QStringLiteral("moduleId")).toString() != module_id) continue;
    item.insert(QStringLiteral("linkedGame"), QString{});
    fixture_modules_[i] = item;
    emit changed();
    return ServiceResult::success(QStringLiteral("Game link removed"),
                                  QStringLiteral("The fixture module is no longer linked to a game in this test session."));
  }
  return ServiceResult::failure(QStringLiteral("Module game link"), QStringLiteral("The selected module no longer exists."));
}

}  // namespace xenon::launcher::frontend_backend
