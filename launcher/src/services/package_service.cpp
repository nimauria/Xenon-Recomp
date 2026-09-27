#include "package_service.hpp"

#include "path_service.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVariantMap>

namespace xenon::launcher {

PackageService::PackageService(PathService& paths) : paths_(paths) {}

ServiceResult PackageService::stage(const QUrl& source, const QString& category) const {
  const auto local = source.isLocalFile() ? source.toLocalFile() : source.toString();
  const QFileInfo info{local};
  if (!info.exists() || !info.isFile()) {
    return ServiceResult::failure(QStringLiteral("Package import"),
                                  QStringLiteral("The selected package could not be read."));
  }

  auto safe_category = category.trimmed().isEmpty() ? QStringLiteral("incoming")
                                                     : QDir::cleanPath(category.trimmed());
  if (QDir::isAbsolutePath(safe_category) || safe_category == QStringLiteral("..") ||
      safe_category.startsWith(QStringLiteral("../")) ||
      safe_category.startsWith(QStringLiteral("..\\"))) {
    return ServiceResult::failure(QStringLiteral("Package import"),
                                  QStringLiteral("The package staging category is invalid."));
  }
  const auto target_dir = QDir{paths_.packagesPath()}.filePath(safe_category);
  if (!paths_.ensureDirectory(target_dir)) {
    return ServiceResult::failure(QStringLiteral("Package import"),
                                  QStringLiteral("Xenon could not create its package staging directory."));
  }

  auto target = QDir{target_dir}.filePath(info.fileName());
  if (QFileInfo::exists(target)) {
    const auto stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"));
    const auto suffix = info.suffix();
    const auto renamed = suffix.isEmpty()
        ? QStringLiteral("%1-%2").arg(info.completeBaseName(), stamp)
        : QStringLiteral("%1-%2.%3").arg(info.completeBaseName(), stamp, suffix);
    target = QDir{target_dir}.filePath(renamed);
  }
  if (!QFile::copy(local, target)) {
    return ServiceResult::failure(QStringLiteral("Package import"),
                                  QStringLiteral("Xenon could not copy the package into staging."));
  }
  return ServiceResult::success(QStringLiteral("Package staged"),
                                QStringLiteral("The package was copied into Xenon's local staging area."),
                                target);
}


ServiceResult PackageService::allocateStagingPath(const QString& category, const QString& file_name) const {
  auto safe_category = category.trimmed().isEmpty() ? QStringLiteral("incoming")
                                                     : QDir::cleanPath(category.trimmed());
  if (QDir::isAbsolutePath(safe_category) || safe_category == QStringLiteral("..") ||
      safe_category.startsWith(QStringLiteral("../")) ||
      safe_category.startsWith(QStringLiteral("..\\"))) {
    return ServiceResult::failure(QStringLiteral("Package staging"),
                                  QStringLiteral("The package staging category is invalid."));
  }

  const auto safe_name = QFileInfo{file_name.trimmed()}.fileName();
  if (safe_name.isEmpty() || safe_name == QStringLiteral(".") || safe_name == QStringLiteral("..")) {
    return ServiceResult::failure(QStringLiteral("Package staging"),
                                  QStringLiteral("The package filename is invalid."));
  }

  const auto target_dir = QDir{paths_.packagesPath()}.filePath(safe_category);
  if (!paths_.ensureDirectory(target_dir)) {
    return ServiceResult::failure(QStringLiteral("Package staging"),
                                  QStringLiteral("Xenon could not create its package staging directory."));
  }

  auto target = QDir{target_dir}.filePath(safe_name);
  if (QFileInfo::exists(target)) QFile::remove(target);
  return ServiceResult::success({}, {}, target);
}

QVariantList PackageService::stagedPackages(const QString& category) const {
  QVariantList result;
  const auto path = QDir{paths_.packagesPath()}.filePath(category);
  const QDir dir{path};
  const auto files = dir.entryInfoList(QDir::Files, QDir::Time);
  for (const auto& file : files) {
    QVariantMap item;
    item.insert(QStringLiteral("name"), file.fileName());
    item.insert(QStringLiteral("path"), file.absoluteFilePath());
    item.insert(QStringLiteral("size"), file.size());
    item.insert(QStringLiteral("modified"), file.lastModified());
    result.append(item);
  }
  return result;
}

}  // namespace xenon::launcher
