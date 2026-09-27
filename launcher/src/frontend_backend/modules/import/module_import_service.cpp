#include "module_import_service.hpp"

#include <QFileInfo>
#include <QStringList>

namespace xenon::launcher::frontend_backend {

ModuleImportService::ModuleImportService(ModuleService& modules, QObject* parent)
    : QObject(parent), modules_(modules), package_installer_(modules_) {}

QString ModuleImportService::manifestModuleId(const QVariantMap& manifest) {
  auto id = manifest.value(QStringLiteral("id")).toString().trimmed();
  if (id.isEmpty()) id = manifest.value(QStringLiteral("moduleId")).toString().trimmed();
  if (id.isEmpty()) id = manifest.value(QStringLiteral("module_id")).toString().trimmed();
  return id;
}

ServiceResult ModuleImportService::importDirectory(const QString& directory) {
  const auto manifest = modules_.inspectDirectory(directory);
  if (manifest.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module import"),
                                  QStringLiteral("The selected folder does not contain a supported Xenon module manifest."));
  }
  const auto id = manifestModuleId(manifest);
  if (id.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module import"),
                                  QStringLiteral("The selected module manifest does not declare a module ID."));
  }
  return modules_.installFromDirectory(id, directory, false);
}

ServiceResult ModuleImportService::importSources(const QList<QUrl>& sources) {
  if (sources.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module import"),
                                  QStringLiteral("No module package or folder was selected."));
  }

  int installed = 0;
  QStringList failures;
  for (const auto& source : sources) {
    if (!source.isLocalFile()) {
      failures.append(QStringLiteral("Only local module packages can be imported."));
      continue;
    }
    const QFileInfo info{source.toLocalFile()};
    if (!info.exists() || info.isSymLink()) {
      failures.append(QStringLiteral("%1 is unavailable or unsafe.").arg(info.fileName()));
      continue;
    }

    ServiceResult result;
    if (info.isDir()) {
      result = importDirectory(info.absoluteFilePath());
    } else if (info.isFile() && info.fileName().endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
      result = package_installer_.installDiscovered(info.absoluteFilePath());
    } else {
      result = ServiceResult::failure(
          QStringLiteral("Module import"),
          QStringLiteral("%1 is not a supported unpacked module or ZIP package.").arg(info.fileName()));
    }

    if (result.ok) {
      ++installed;
    } else if (!result.message.trimmed().isEmpty()) {
      failures.append(result.message);
    }
  }

  if (installed == 0) {
    return ServiceResult::failure(QStringLiteral("Module import failed"),
                                  failures.isEmpty() ? QStringLiteral("No modules were installed.")
                                                     : failures.join(QStringLiteral("\n")));
  }

  QString message = QStringLiteral("Installed %1 module%2 into the managed Modules directory.")
                        .arg(installed)
                        .arg(installed == 1 ? QString{} : QStringLiteral("s"));
  if (!failures.isEmpty()) {
    message += QStringLiteral(" %1 selected item%2 could not be imported.")
                   .arg(failures.size())
                   .arg(failures.size() == 1 ? QString{} : QStringLiteral("s"));
  }
  return ServiceResult::success(QStringLiteral("Module import complete"), message);
}

}  // namespace xenon::launcher::frontend_backend
