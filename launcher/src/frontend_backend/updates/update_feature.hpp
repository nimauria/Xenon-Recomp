#pragma once

#include "../../services/package_service.hpp"
#include "../../services/service_result.hpp"
#include "install/update_installer.hpp"
#include "model/update_release.hpp"

#include <QObject>
#include <QUrl>
#include <QVariantMap>

#include <memory>

namespace xenon::launcher::frontend_backend {
class SettingsFeature;
class UpdateProvider;

class UpdateFeature final : public QObject {
  Q_OBJECT

 public:
  UpdateFeature(PackageService& packages, SettingsFeature& settings,
                QObject* parent = nullptr);
  ~UpdateFeature() override;

  void initialize();

  [[nodiscard]] QVariantMap launcherState() const;
  [[nodiscard]] ServiceResult checkLauncher(bool manual = true);
  [[nodiscard]] ServiceResult downloadLauncher();
  [[nodiscard]] ServiceResult installLauncher();
  [[nodiscard]] ServiceResult cancelLauncherDownload();
  [[nodiscard]] ServiceResult stageLauncherPackage(const QUrl& source);

 signals:
  void changed();
  void notificationRequested(const QString& title, const QString& message);
  void restartRequested();

 private:
  void connectProvider();
  void setStatus(const QString& status, const QString& message = {});
  void handleCheckSucceeded(const UpdateRelease& release);
  void handleCheckFailed(const QString& message);
  void handleDownloadSucceeded(const QString& path, const UpdateRelease& release);
  void handleDownloadFailed(const QString& message);
  [[nodiscard]] bool automaticCheckDue() const;
  [[nodiscard]] static qint64 intervalSeconds(const QString& interval);

  PackageService& packages_;
  SettingsFeature& settings_;
  std::unique_ptr<UpdateProvider> provider_;
  UpdateInstaller installer_;
  QVariantMap launcher_state_;
  UpdateRelease available_release_;
  bool manual_check_ = false;
};

}  // namespace xenon::launcher::frontend_backend
