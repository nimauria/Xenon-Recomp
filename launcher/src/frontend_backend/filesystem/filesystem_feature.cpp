#include "filesystem_feature.hpp"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QUrl>

namespace xenon::launcher::frontend_backend {

FilesystemFeature::FilesystemFeature(PathService& paths) : paths_(paths) {}

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

}  // namespace xenon::launcher::frontend_backend
