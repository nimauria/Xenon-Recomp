#pragma once

#include "service_result.hpp"

#include <QUrl>
#include <QVariantList>

namespace xenon::launcher {

class PathService;

class PackageService final {
 public:
  explicit PackageService(PathService& paths);

  [[nodiscard]] ServiceResult stage(const QUrl& source, const QString& category) const;
  [[nodiscard]] ServiceResult allocateStagingPath(const QString& category, const QString& file_name) const;
  [[nodiscard]] QVariantList stagedPackages(const QString& category) const;

 private:
  PathService& paths_;
};

}  // namespace xenon::launcher
