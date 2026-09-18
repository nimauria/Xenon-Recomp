#include "content_import_service.hpp"

#include "dlc_service.hpp"
#include "library_service.hpp"
#include "module_service.hpp"
#include "../runtime/content_probe.hpp"

#include <QFileInfo>
#include <QVariantMap>

namespace xenon::launcher {
namespace {
bool validLocalSource(const QUrl& source) {
  return source.isLocalFile() && QFileInfo::exists(source.toLocalFile());
}
}  // namespace

ContentImportService::ContentImportService(LibraryService& library, ModuleService& modules,
                                           DlcService& dlc, IContentProbe& probe)
    : library_(library), modules_(modules), dlc_(dlc), probe_(probe) {}

bool ContentImportService::probeAvailable() const noexcept { return probe_.available(); }

QString ContentImportService::probeStatus() const { return probe_.status(); }

ServiceResult ContentImportService::importGameContent(const QList<QUrl>& sources) {
  if (sources.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Game import"),
                                  QStringLiteral("No game content was selected."));
  }
  if (!probe_.available()) {
    return ServiceResult::failure(
        QStringLiteral("Game importer pending"),
        QStringLiteral("The Library UI and import pipeline are ready, but game identification "
                       "is waiting for the Xenon filesystem/content probe. No library data was changed."));
  }

  int imported = 0;
  int failed = 0;
  QVariantList imported_ids;
  QVariantList module_candidates;
  for (const auto& value : modules_.modules()) {
    auto candidate = value.toMap();
    candidate.insert(QStringLiteral("manifest"),
                     modules_.manifest(candidate.value(QStringLiteral("moduleId")).toString()));
    module_candidates.append(candidate);
  }

  for (const auto& source : sources) {
    if (!validLocalSource(source)) {
      ++failed;
      continue;
    }
    const auto identification = probe_.identifyGame(source, module_candidates);
    const auto identified = identification.data.toMap();
    const auto module_id = identified.value(QStringLiteral("moduleId")).toString().trimmed();
    if (!identification.ok || identified.isEmpty() || module_id.isEmpty() ||
        modules_.module(module_id).isEmpty()) {
      ++failed;
      continue;
    }
    const auto registered = library_.registerIdentifiedContent(source, identified);
    if (!registered.ok) {
      ++failed;
      continue;
    }
    ++imported;
    imported_ids.append(registered.data);
  }

  if (imported == 0) {
    return ServiceResult::failure(
        QStringLiteral("Game import"),
        QStringLiteral("The Xenon content probe could not identify and register any selected content."));
  }

  QString message = QStringLiteral("Imported %1 identified game item(s).").arg(imported);
  if (failed > 0) message += QStringLiteral(" %1 item(s) could not be imported.").arg(failed);
  return ServiceResult::success(QStringLiteral("Library updated"), message, imported_ids);
}

ServiceResult ContentImportService::importDlcContent(const QString& game_id,
                                                      const QList<QUrl>& sources,
                                                      const QString& expected_dlc_id) {
  const auto game = library_.entry(game_id);
  if (game.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("DLC import"),
                                  QStringLiteral("Select a valid library entry before importing DLC."));
  }
  if (sources.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("DLC import"),
                                  QStringLiteral("No add-on content was selected."));
  }

  const auto module_id = game.value(QStringLiteral("moduleId")).toString();
  const auto catalogue = modules_.dlcCatalog(module_id);
  if (catalogue.isEmpty()) {
    return ServiceResult::failure(
        QStringLiteral("DLC import"),
        QStringLiteral("The selected game module does not declare any DLC entries."));
  }
  QVariantList probe_catalogue = catalogue;
  const auto expected_id = expected_dlc_id.trimmed();
  if (!expected_id.isEmpty()) {
    probe_catalogue.clear();
    for (const auto& value : catalogue) {
      if (value.toMap().value(QStringLiteral("dlcId")).toString() == expected_id) {
        probe_catalogue.append(value);
        break;
      }
    }
    if (probe_catalogue.isEmpty()) {
      return ServiceResult::failure(
          QStringLiteral("DLC import"),
          QStringLiteral("The selected DLC entry is no longer declared by this game's module."));
    }
  }

  if (!probe_.available()) {
    return ServiceResult::failure(
        QStringLiteral("DLC importer pending"),
        QStringLiteral("The module DLC catalogue and Xenon-managed DLC folders are ready, but "
                       "package identification is waiting for the Xenon filesystem/content probe. "
                       "No files were changed."));
  }

  int installed = 0;
  int failed = 0;
  QVariantList installed_ids;

  for (const auto& source : sources) {
    if (!validLocalSource(source)) {
      ++failed;
      continue;
    }

    const auto identification = probe_.identifyDlc(source, game_id, probe_catalogue);
    const auto identified = identification.data.toMap();
    const auto dlc_id = identified.value(QStringLiteral("dlcId")).toString().trimmed();
    if (!identification.ok || dlc_id.isEmpty() ||
        (!expected_id.isEmpty() && dlc_id != expected_id) ||
        dlc_.entry(game_id, dlc_id).isEmpty()) {
      ++failed;
      continue;
    }

    const auto existing_dlc = dlc_.entry(game_id, dlc_id);
    if (existing_dlc.value(QStringLiteral("installed")).toBool()) {
      ++failed;
      continue;
    }

    const auto prepared = dlc_.prepareInstall(game_id, dlc_id);
    const auto destination = prepared.data.toString();
    if (!prepared.ok || destination.isEmpty()) {
      ++failed;
      continue;
    }

    const auto materialized = probe_.materializeDlc(source, destination, identified);
    if (!materialized.ok) {
      (void)dlc_.remove(game_id, dlc_id);  // safe managed-path cleanup, including partial copies
      ++failed;
      continue;
    }

    QVariantMap receipt = identified.value(QStringLiteral("receipt")).toMap();
    receipt.insert(QStringLiteral("sourceName"), QFileInfo{source.toLocalFile()}.fileName());
    const auto committed = dlc_.commitInstall(game_id, dlc_id, receipt);
    if (!committed.ok) {
      (void)dlc_.remove(game_id, dlc_id);
      ++failed;
      continue;
    }

    ++installed;
    installed_ids.append(dlc_id);
  }

  if (installed == 0) {
    return ServiceResult::failure(
        QStringLiteral("DLC import"),
        QStringLiteral("No selected package could be identified and installed as DLC for this game."));
  }

  QString message = QStringLiteral("Installed %1 DLC item(s) into Xenon-managed folders.").arg(installed);
  if (failed > 0) message += QStringLiteral(" %1 item(s) could not be installed.").arg(failed);
  return ServiceResult::success(QStringLiteral("DLC import complete"), message, installed_ids);
}

}  // namespace xenon::launcher
