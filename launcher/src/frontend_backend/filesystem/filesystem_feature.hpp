#pragma once

#include "../../services/filesystem_service.hpp"
#include "../../services/path_service.hpp"
#include "../../services/service_result.hpp"

#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {

class FilesystemFeature final {
 public:
  explicit FilesystemFeature(PathService& paths, FilesystemService& filesystem);

  [[nodiscard]] QString toLocalPath(const QUrl& url) const;
  [[nodiscard]] bool canOpenPath(const QString& path) const;
  [[nodiscard]] ServiceResult openFolder(const QString& path) const;
  [[nodiscard]] ServiceResult openExternalUrl(const QString& url) const;
  [[nodiscard]] ServiceResult copyText(const QString& text) const;

  // VFS management exposed to frontend
  [[nodiscard]] QVariantList getMounts() const;
  [[nodiscard]] QVariantList getSymbolicLinks() const;
  [[nodiscard]] QString getWorkingDirectory() const;
  [[nodiscard]] QVariantMap getFilesystemStatus() const;
  
  [[nodiscard]] ServiceResult mountHostPath(const QString& mount_point,
                                           const QString& host_path,
                                           bool read_only = false) const;
  [[nodiscard]] ServiceResult mountGdfxImage(const QString& mount_point,
                                            const QString& image_path) const;
  [[nodiscard]] ServiceResult mountStfsPackage(const QString& mount_point,
                                              const QString& package_path) const;
  [[nodiscard]] ServiceResult unmount(const QString& mount_point) const;
  [[nodiscard]] ServiceResult registerSymbolicLink(const QString& alias,
                                                  const QString& target) const;
  [[nodiscard]] ServiceResult unregisterSymbolicLink(const QString& alias) const;
  [[nodiscard]] ServiceResult setWorkingDirectory(const QString& guest_path) const;
  [[nodiscard]] ServiceResult testPath(const QString& guest_path) const;

 private:
  PathService& paths_;
  FilesystemService& filesystem_;
};

}  // namespace xenon::launcher::frontend_backend
