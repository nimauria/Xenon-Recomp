#include "module_package_installer.hpp"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

namespace xenon::launcher::frontend_backend {
namespace {
QString manifestModuleId(const QVariantMap& manifest) {
  auto id = manifest.value(QStringLiteral("id")).toString().trimmed();
  if (id.isEmpty()) id = manifest.value(QStringLiteral("moduleId")).toString().trimmed();
  if (id.isEmpty()) id = manifest.value(QStringLiteral("module_id")).toString().trimmed();
  return id;
}
}  // namespace

bool ModulePackageInstaller::platformSupported() {
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
  return true;
#else
  return false;
#endif
}

ServiceResult ModulePackageInstaller::extractArchive(const QString& archive_path,
                                                      const QString& destination) const {
  if (!archive_path.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
    return ServiceResult::failure(
        QStringLiteral("Module install"),
        QStringLiteral("This Xenon build currently installs module archives in ZIP format only."));
  }

  QProcess process;
#if defined(Q_OS_WIN)
  const auto command = QStringLiteral(
      "Expand-Archive -LiteralPath $args[0] -DestinationPath $args[1] -Force");
  process.start(QStringLiteral("powershell.exe"),
                {QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"),
                 QStringLiteral("-NonInteractive"), QStringLiteral("-Command"), command,
                 archive_path, destination});
#elif defined(Q_OS_LINUX)
  process.start(QStringLiteral("unzip"),
                {QStringLiteral("-qq"), archive_path, QStringLiteral("-d"), destination});
#elif defined(Q_OS_MACOS)
  process.start(QStringLiteral("ditto"),
                {QStringLiteral("-x"), QStringLiteral("-k"), archive_path, destination});
#else
  return ServiceResult::failure(QStringLiteral("Module install"),
                                QStringLiteral("Automatic module archive extraction is unavailable on this platform."));
#endif

  if (!process.waitForStarted(5000)) {
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("Xenon could not start the platform archive extractor."));
  }
  if (!process.waitForFinished(120000)) {
    process.kill();
    process.waitForFinished(2000);
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("The module archive extractor timed out."));
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    auto detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (detail.isEmpty()) detail = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    return ServiceResult::failure(
        QStringLiteral("Module install"),
        detail.isEmpty() ? QStringLiteral("The platform archive extractor failed.")
                         : QStringLiteral("The platform archive extractor failed: %1").arg(detail));
  }
  return ServiceResult::success();
}

QString ModulePackageInstaller::locateModuleRoot(const QString& extracted_root,
                                                  const QString& expected_module_id) const {
  const auto root_manifest = modules_.inspectDirectory(extracted_root);
  if (!root_manifest.isEmpty()) {
    const auto id = manifestModuleId(root_manifest);
    if (id == expected_module_id) return extracted_root;
  }

  const QDir root{extracted_root};
  const auto children = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  QString match;
  for (const auto& child : children) {
    if (child.isSymLink()) continue;
    const auto manifest = modules_.inspectDirectory(child.absoluteFilePath());
    if (manifest.isEmpty()) continue;
    const auto id = manifestModuleId(manifest);
    if (id != expected_module_id) continue;
    if (!match.isEmpty()) return {};
    match = child.absoluteFilePath();
  }
  return match;
}

ServiceResult ModulePackageInstaller::locateSingleModule(const QString& extracted_root) const {
  QVariantList candidates;
  const auto addCandidate = [this, &candidates](const QString& path) {
    const auto manifest = modules_.inspectDirectory(path);
    if (manifest.isEmpty()) return;
    const auto id = manifestModuleId(manifest);
    if (id.isEmpty()) return;
    candidates.append(QVariantMap{{QStringLiteral("moduleId"), id},
                                  {QStringLiteral("path"), path}});
  };

  addCandidate(extracted_root);
  const QDir root{extracted_root};
  for (const auto& child : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
    if (!child.isSymLink()) addCandidate(child.absoluteFilePath());
  }

  if (candidates.size() != 1) {
    return ServiceResult::failure(
        QStringLiteral("Module import"),
        candidates.isEmpty()
            ? QStringLiteral("The package does not contain a supported Xenon module manifest at its root or in one top-level directory.")
            : QStringLiteral("The package contains more than one Xenon module. Import one module package at a time."));
  }
  return ServiceResult::success({}, {}, candidates.first());
}

ServiceResult ModulePackageInstaller::installDiscovered(const QString& archive_path) const {
  if (!platformSupported()) {
    return ServiceResult::failure(QStringLiteral("Module import"),
                                  QStringLiteral("Automatic module package extraction is unavailable on this platform."));
  }
  const QFileInfo archive{archive_path};
  if (!archive.exists() || !archive.isFile() || archive.isSymLink()) {
    return ServiceResult::failure(QStringLiteral("Module import"),
                                  QStringLiteral("The selected module package is unavailable."));
  }

  QTemporaryDir extracted;
  if (!extracted.isValid()) {
    return ServiceResult::failure(QStringLiteral("Module import"),
                                  QStringLiteral("Xenon could not create a temporary extraction directory."));
  }
  const auto extracted_result = extractArchive(archive.absoluteFilePath(), extracted.path());
  if (!extracted_result.ok) return extracted_result;

  const auto located = locateSingleModule(extracted.path());
  if (!located.ok) return located;
  const auto info = located.data.toMap();
  const auto module_id = info.value(QStringLiteral("moduleId")).toString();
  const auto module_root = info.value(QStringLiteral("path")).toString();
  auto result = modules_.installFromDirectory(module_id, module_root, false);
  if (result.ok) result.data = module_id;
  return result;
}

ServiceResult ModulePackageInstaller::install(const QString& module_id,
                                               const QString& archive_path) const {
  if (!platformSupported()) {
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("Automatic module installation is unavailable on this platform."));
  }
  const QFileInfo archive{archive_path};
  if (!archive.exists() || !archive.isFile()) {
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("The verified module package is no longer available."));
  }

  QTemporaryDir extracted;
  if (!extracted.isValid()) {
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("Xenon could not create a temporary extraction directory."));
  }

  const auto extracted_result = extractArchive(archive.absoluteFilePath(), extracted.path());
  if (!extracted_result.ok) return extracted_result;

  const auto module_root = locateModuleRoot(extracted.path(), module_id);
  if (module_root.isEmpty()) {
    return ServiceResult::failure(
        QStringLiteral("Module install"),
        QStringLiteral("The verified package did not contain exactly one module matching %1.")
            .arg(module_id));
  }

  return modules_.installFromDirectory(module_id, module_root, true);
}

}  // namespace xenon::launcher::frontend_backend
