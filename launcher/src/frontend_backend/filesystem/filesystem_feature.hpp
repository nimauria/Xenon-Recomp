#pragma once

#include "../../services/path_service.hpp"
#include "../../services/service_result.hpp"

#include <QUrl>

namespace xenon::launcher::frontend_backend {

class FilesystemFeature final {
 public:
  explicit FilesystemFeature(PathService& paths);

  [[nodiscard]] QString toLocalPath(const QUrl& url) const;
  [[nodiscard]] bool canOpenPath(const QString& path) const;
  [[nodiscard]] ServiceResult openFolder(const QString& path) const;
  [[nodiscard]] ServiceResult openExternalUrl(const QString& url) const;
  [[nodiscard]] ServiceResult copyText(const QString& text) const;

 private:
  PathService& paths_;
};

}  // namespace xenon::launcher::frontend_backend
