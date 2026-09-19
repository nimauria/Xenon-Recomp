#pragma once

#include "../../../services/module_service.hpp"
#include "../../../services/package_service.hpp"
#include "../../../services/service_result.hpp"
#include "../../updates/model/update_release.hpp"
#include "../package/module_package_installer.hpp"
#include "history/module_update_history_store.hpp"

#include <QCryptographicHash>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QSaveFile>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

class QNetworkReply;

namespace xenon::launcher::frontend_backend {

class ModuleUpdateService final : public QObject {
  Q_OBJECT

 public:
  ModuleUpdateService(PackageService& packages, ModuleService& modules,
                      QObject* parent = nullptr);

  [[nodiscard]] QVariantMap state(const QString& module_id) const;
  [[nodiscard]] QVariantMap states() const;
  [[nodiscard]] QVariantList history(const QString& module_id = {}) const;

  void check(const QString& module_id, const QVariantMap& catalog_entry,
             const QString& installed_version, bool include_prerelease);
  [[nodiscard]] ServiceResult download(const QString& module_id);
  [[nodiscard]] ServiceResult cancelDownload(const QString& module_id);
  [[nodiscard]] ServiceResult install(const QString& module_id);
  [[nodiscard]] ServiceResult rollback(const QString& module_id);
  [[nodiscard]] ServiceResult clearHistory(const QString& module_id = {});

 signals:
  void changed(const QString& module_id);
  void historyChanged(const QString& module_id);
  void moduleInstalled(const QString& module_id);
  void notificationRequested(const QString& title, const QString& message);

 private:
  [[nodiscard]] QNetworkRequest apiRequest(const QUrl& url) const;
  [[nodiscard]] QNetworkRequest assetRequest(const QUrl& url) const;
  [[nodiscard]] UpdateRelease parseBestRelease(const QByteArray& payload,
                                               const QString& expected_asset,
                                               bool include_prerelease,
                                               QString* error) const;
  [[nodiscard]] static QString digestHex(const QString& digest);
  void setState(const QString& module_id, const QString& status,
                const QString& message = {});
  void refreshRollbackState(const QString& module_id);
  void handleCheckSucceeded(const QString& module_id, const UpdateRelease& release,
                            const QString& installed_version);
  void recordHistory(const QString& module_id, const QString& action,
                     const QString& outcome, const QString& from_version,
                     const QString& to_version, const QString& message,
                     const QVariantMap& metadata = {});

  PackageService& packages_;
  ModuleService& modules_;
  ModulePackageInstaller installer_;
  ModuleUpdateHistoryStore history_;
  QNetworkAccessManager network_;
  QHash<QString, QVariantMap> states_;
  QHash<QString, UpdateRelease> releases_;
  QHash<QString, QVariantMap> catalog_entries_;

  QPointer<QNetworkReply> active_download_;
  QString active_download_module_;
  std::unique_ptr<QSaveFile> download_file_;
  std::unique_ptr<QCryptographicHash> download_hash_;
  QString download_target_;
  UpdateRelease download_release_;
  bool cancel_requested_ = false;
};

}  // namespace xenon::launcher::frontend_backend
