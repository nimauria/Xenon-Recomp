#pragma once

#include "../../../services/module_service.hpp"
#include "../../../services/service_result.hpp"

#include <QString>

namespace xenon::launcher::frontend_backend {

class ModulePackageInstaller final {
 public:
  explicit ModulePackageInstaller(ModuleService& modules) : modules_(modules) {}

  [[nodiscard]] ServiceResult install(const QString& module_id, const QString& archive_path,
                                      const QString& expected_version = {},
                                      bool retain_rollback = true) const;
  [[nodiscard]] ServiceResult installDiscovered(const QString& archive_path) const;
  [[nodiscard]] static bool platformSupported();

 private:
  [[nodiscard]] ServiceResult extractArchive(const QString& archive_path,
                                             const QString& destination) const;
  [[nodiscard]] QString locateModuleRoot(const QString& extracted_root,
                                         const QString& expected_module_id) const;
  [[nodiscard]] ServiceResult locateSingleModule(const QString& extracted_root) const;

  ModuleService& modules_;
};

}  // namespace xenon::launcher::frontend_backend
