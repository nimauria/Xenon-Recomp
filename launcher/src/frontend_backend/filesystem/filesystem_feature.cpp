#include "filesystem_feature.hpp"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QUrl>

namespace xenon::launcher::frontend_backend {

FilesystemFeature::FilesystemFeature(PathService& paths, FilesystemService& filesystem)
    : paths_(paths), filesystem_(filesystem) {}

QString FilesystemFeature::toLocalPath(const QUrl& url) const {
  return url.isLocalFile() ? url.toLocalFile() : url.toString();
}

bool FilesystemFeature::canOpenPath(const QString& path) const {
  const auto requested = path.trimmed();
  return !requested.isEmpty() && !requested.contains(QStringLiteral("://"));
}

ServiceResult FilesystemFeature::openFolder(const QString& path) const {
  const auto requested = path.trimmed();
  if (!canOpenPath(requested)) {
    return ServiceResult::failure(QStringLiteral("Unable to open folder"),
                                  QStringLiteral("No local folder path has been configured."));
  }
  const auto clean = QDir::cleanPath(requested);
  if (!paths_.ensureDirectory(clean)) {
    return ServiceResult::failure(QStringLiteral("Unable to open folder"),
                                  QStringLiteral("Xenon could not create or access:\n%1").arg(clean));
  }
  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(clean))) {
    return ServiceResult::failure(QStringLiteral("Unable to open folder"),
                                  QStringLiteral("The operating system could not open:\n%1").arg(clean));
  }
  return ServiceResult::success();
}

ServiceResult FilesystemFeature::openExternalUrl(const QString& url) const {
  const QUrl target{url.trimmed()};
  if (!target.isValid() || (target.scheme() != QStringLiteral("https") &&
                            target.scheme() != QStringLiteral("http"))) {
    return ServiceResult::failure(QStringLiteral("Unable to open link"),
                                  QStringLiteral("The requested web address is invalid."));
  }
  if (!QDesktopServices::openUrl(target)) {
    return ServiceResult::failure(QStringLiteral("Unable to open link"),
                                  QStringLiteral("The operating system could not open the requested address."));
  }
  return ServiceResult::success();
}

ServiceResult FilesystemFeature::copyText(const QString& text) const {
  auto* clipboard = QGuiApplication::clipboard();
  if (clipboard == nullptr) {
    return ServiceResult::failure(QStringLiteral("Clipboard"),
                                  QStringLiteral("The system clipboard is not available."));
  }
  clipboard->setText(text);
  return ServiceResult::success();
}

QVariantList FilesystemFeature::getMounts() const {
  return filesystem_.getMounts();
}

QVariantList FilesystemFeature::getSymbolicLinks() const {
  return filesystem_.getSymbolicLinks();
}

QString FilesystemFeature::getWorkingDirectory() const {
  return filesystem_.getWorkingDirectory();
}

QVariantMap FilesystemFeature::getFilesystemStatus() const {
  return filesystem_.getFilesystemStatus();
}

ServiceResult FilesystemFeature::mountHostPath(const QString& mount_point,
                                               const QString& host_path,
                                               bool read_only) const {
  return filesystem_.mountHostPath(mount_point, host_path, read_only);
}

ServiceResult FilesystemFeature::mountGdfxImage(const QString& mount_point,
                                                const QString& image_path) const {
  return filesystem_.mountGdfxImage(mount_point, image_path);
}

ServiceResult FilesystemFeature::mountStfsPackage(const QString& mount_point,
                                                  const QString& package_path) const {
  return filesystem_.mountStfsPackage(mount_point, package_path);
}

ServiceResult FilesystemFeature::unmount(const QString& mount_point) const {
  return filesystem_.unmount(mount_point);
}

ServiceResult FilesystemFeature::registerSymbolicLink(const QString& alias,
                                                     const QString& target) const {
  return filesystem_.registerSymbolicLink(alias, target);
}

ServiceResult FilesystemFeature::unregisterSymbolicLink(const QString& alias) const {
  return filesystem_.unregisterSymbolicLink(alias);
}

ServiceResult FilesystemFeature::setWorkingDirectory(const QString& guest_path) const {
  return filesystem_.setWorkingDirectory(guest_path);
}

ServiceResult FilesystemFeature::testPath(const QString& guest_path) const {
  return filesystem_.testPath(guest_path);
}

}  // namespace xenon::launcher::frontend_backend
