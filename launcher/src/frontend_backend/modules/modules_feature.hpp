#pragma once

#include "../../services/library_service.hpp"
#include "../../services/module_service.hpp"
#include "../../services/package_service.hpp"
#include "../../services/service_result.hpp"
#include "actions/module_action_catalog.hpp"
#include "catalog/github_module_catalog_provider.hpp"
#include "catalog/assets/module_catalog_asset_cache.hpp"
#include "import/module_import_service.hpp"
#include "updates/module_update_service.hpp"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {
class SettingsFeature;

class ModulesFeature final : public QObject {
  Q_OBJECT

 public:
  ModulesFeature(ModuleService& modules, LibraryService& library, PackageService& packages,
                 PathService& paths, SettingsFeature& settings, bool test_mode,
                 bool suppress_startup_external_work = false, QObject* parent = nullptr);

  [[nodiscard]] QVariantList entries() const;
  [[nodiscard]] QVariantMap module(const QString& module_id) const;
  [[nodiscard]] QVariantList actions(const QString& module_id) const;
  [[nodiscard]] QVariantList pageActions() const;
  [[nodiscard]] ServiceResult refresh();
  [[nodiscard]] ServiceResult importPackages(const QList<QUrl>& sources);
  [[nodiscard]] ServiceResult setEnabled(const QString& module_id, bool enabled);
  [[nodiscard]] ServiceResult remove(const QString& module_id);
  [[nodiscard]] ServiceResult verify(const QString& module_id) const;
  [[nodiscard]] QString modulePath(const QString& module_id) const;
  [[nodiscard]] QVariantList settingsSchema(const QString& module_id) const;
  [[nodiscard]] QVariantMap settingsValues(const QString& module_id) const;
  [[nodiscard]] ServiceResult setSetting(const QString& module_id, const QString& setting_id,
                                         const QVariant& value);

  [[nodiscard]] QVariantList catalogEntries() const;
  [[nodiscard]] QVariantMap catalogEntryData(const QString& module_id) const;
  [[nodiscard]] QVariantMap launcherMetadata(const QString& module_id) const;
  [[nodiscard]] QVariantList catalogDlc(const QString& module_id) const;
  [[nodiscard]] ServiceResult refreshPresentationMetadata(const QString& module_id);
  [[nodiscard]] QVariantMap catalogState() const;
  [[nodiscard]] QVariantMap updateState(const QString& module_id) const;
  [[nodiscard]] QVariantList updateHistory(const QString& module_id) const;
  [[nodiscard]] ServiceResult refreshCatalog();
  [[nodiscard]] ServiceResult checkForUpdate(const QString& module_id);
  [[nodiscard]] ServiceResult checkAllUpdates();
  [[nodiscard]] ServiceResult downloadUpdate(const QString& module_id);
  [[nodiscard]] ServiceResult cancelUpdateDownload(const QString& module_id);
  [[nodiscard]] ServiceResult installUpdate(const QString& module_id);
  [[nodiscard]] ServiceResult rollbackUpdate(const QString& module_id);
  [[nodiscard]] ServiceResult clearUpdateHistory(const QString& module_id);
  [[nodiscard]] ServiceResult requestInstall(const QString& module_id);
  [[nodiscard]] ServiceResult requestUpdate(const QString& module_id);
  [[nodiscard]] ServiceResult unlinkGame(const QString& module_id);

 signals:
  void changed();
  void catalogChanged();
  void updateStateChanged(const QString& module_id);
  void updateHistoryChanged(const QString& module_id);
  void notificationRequested(const QString& title, const QString& message);

 private:
  [[nodiscard]] QString fixtureMode() const;
  void rebuildFixtures();
  [[nodiscard]] QVariantList genericSettings(const QString& module_id) const;
  [[nodiscard]] QVariantList gracemeriaSettings(const QString& module_id) const;
  [[nodiscard]] QVariantList applyFixtureValues(const QString& module_id, QVariantList schema) const;
  [[nodiscard]] QVariantMap decorated(QVariantMap item) const;
  [[nodiscard]] QVariantMap catalogEntry(const QString& module_id) const;
  [[nodiscard]] QVariantMap fixtureUpdateState(const QString& module_id) const;
  [[nodiscard]] QString installedVersion(const QString& module_id) const;
  void continueInstallRequest(const QString& module_id);
  void syncCatalogAssets();

  ModuleService& modules_;
  LibraryService& library_;
  SettingsFeature& settings_;
  bool test_mode_ = false;
  bool suppress_startup_external_work_ = false;
  GitHubModuleCatalogProvider catalog_;
  ModuleCatalogAssetCache asset_cache_;
  ModuleImportService importer_;
  ModuleUpdateService updater_;
  QVariantList fixture_modules_;
  QHash<QString, QVariantMap> fixture_settings_;
  QHash<QString, QVariantMap> fixture_update_states_;
  QSet<QString> install_requests_;
};

}  // namespace xenon::launcher::frontend_backend
