#pragma once

#include "../../../services/module_service.hpp"
#include "../../../services/service_result.hpp"
#include "../package/module_package_installer.hpp"

#include <QList>
#include <QObject>
#include <QUrl>

namespace xenon::launcher::frontend_backend {

class ModuleImportService final : public QObject {
  Q_OBJECT

 public:
  explicit ModuleImportService(ModuleService& modules, QObject* parent = nullptr);

  [[nodiscard]] ServiceResult importSources(const QList<QUrl>& sources);

 private:
  [[nodiscard]] ServiceResult importDirectory(const QString& directory);
  [[nodiscard]] static QString manifestModuleId(const QVariantMap& manifest);

  ModuleService& modules_;
  ModulePackageInstaller package_installer_;
};

}  // namespace xenon::launcher::frontend_backend
