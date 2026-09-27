#pragma once

#include "service_result.hpp"

#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher {

class LibraryService;
class ModuleService;
class PathService;
class SettingsService;

// Launcher-owned DLC catalogue and storage service.
//
// Module manifests describe which add-ons exist. The launcher owns how those
// add-ons are represented, where their local copies live, and how removal /
// verification works. Identifying arbitrary Xbox 360 packages is deliberately
// delegated to ContentImportService until the Xenon content probe API exists.
class DlcService final : public QObject {
  Q_OBJECT

 public:
  DlcService(SettingsService& settings, PathService& paths, LibraryService& library,
             ModuleService& modules, QObject* parent = nullptr);

  [[nodiscard]] QVariantList entries(const QString& game_id) const;
  [[nodiscard]] QVariantMap entry(const QString& game_id, const QString& dlc_id) const;
  [[nodiscard]] QVariantList launchEntries(const QString& game_id) const;
  [[nodiscard]] QString rootPath(const QString& game_id) const;
  [[nodiscard]] QString ensureRootPath(const QString& game_id) const;
  [[nodiscard]] QString itemPath(const QString& game_id, const QString& dlc_id) const;

  [[nodiscard]] ServiceResult verify(const QString& game_id, const QString& dlc_id) const;
  [[nodiscard]] ServiceResult remove(const QString& game_id, const QString& dlc_id);
  // Framework hand-off used by the future Xbox content importer after it has
  // identified a package as a specific manifest DLC entry. This prepares only
  // launcher-managed storage; the framework/importer still performs content
  // validation and copying before calling commitInstall().
  [[nodiscard]] ServiceResult prepareInstall(const QString& game_id,
                                             const QString& dlc_id) const;
  [[nodiscard]] ServiceResult commitInstall(const QString& game_id, const QString& dlc_id,
                                            const QVariantMap& receipt = {});

 signals:
  void changed(const QString& game_id);

 private:
  [[nodiscard]] QVariantList mergedEntries(const QString& game_id) const;
  [[nodiscard]] QVariantMap moduleDefinition(const QString& game_id,
                                             const QString& dlc_id) const;
  [[nodiscard]] QString folderName(const QString& game_id, const QVariantMap& definition) const;
  [[nodiscard]] QString installReceiptPath(const QString& item_path) const;
  [[nodiscard]] bool directoryHasPayload(const QString& item_path) const;
  [[nodiscard]] bool isSafeManagedPath(const QString& game_id, const QString& candidate) const;
  [[nodiscard]] QVariantMap readReceipt(const QString& item_path) const;
  [[nodiscard]] bool writeReceipt(const QString& item_path, const QVariantMap& receipt) const;

  SettingsService& settings_;
  PathService& paths_;
  LibraryService& library_;
  ModuleService& modules_;
};

}  // namespace xenon::launcher
