#pragma once

#include "service_result.hpp"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher {

class PathService;
class SettingsService;

class ModuleService final : public QObject {
  Q_OBJECT

 public:
  ModuleService(SettingsService& settings, PathService& paths, QObject* parent = nullptr);

  [[nodiscard]] QVariantList modules() const;
  [[nodiscard]] QVariantMap module(const QString& module_id) const;
  [[nodiscard]] QVariantMap manifest(const QString& module_id) const;
  [[nodiscard]] QVariantMap runtimeApiRequirements(const QString& module_id) const;
  [[nodiscard]] QVariantList dlcCatalog(const QString& module_id) const;
  [[nodiscard]] ServiceResult refresh();
  [[nodiscard]] ServiceResult setEnabled(const QString& module_id, bool enabled);
  [[nodiscard]] ServiceResult remove(const QString& module_id);
  [[nodiscard]] ServiceResult installFromDirectory(const QString& expected_module_id,
                                                   const QString& source_directory,
                                                   bool replace_existing = true,
                                                   bool retain_rollback = false);
  [[nodiscard]] QVariantMap latestRollback(const QString& module_id) const;
  [[nodiscard]] ServiceResult restoreLatestRollback(const QString& module_id);
  [[nodiscard]] QVariantMap inspectDirectory(const QString& module_dir) const;
  [[nodiscard]] ServiceResult verify(const QString& module_id) const;
  [[nodiscard]] QString modulePath(const QString& module_id) const;
  // Absolute path to the module's compiled-code native extension library for
  // this host platform (see docs/RUNTIME_HOST.md), or empty if the manifest
  // declares none for this platform. The manifest's "nativeExtension" field
  // may be a single path (applied to any platform) or an object keyed by
  // platform id ("windows-x64", "linux-x64", ...).
  [[nodiscard]] QString nativeExtensionPath(const QString& module_id) const;
  [[nodiscard]] QVariantList settingsSchema(const QString& module_id) const;
  [[nodiscard]] QVariantMap settingsValues(const QString& module_id) const;
  [[nodiscard]] ServiceResult setSetting(const QString& module_id, const QString& setting_id,
                                         const QVariant& value);

 signals:
  void changed();

 private:
  [[nodiscard]] QVariantMap readManifest(const QString& module_dir) const;
  [[nodiscard]] QVariantMap manifestToUi(const QVariantMap& manifest,
                                         const QString& module_dir) const;
  [[nodiscard]] bool copyDirectory(const QString& source, const QString& destination) const;
  [[nodiscard]] QString rollbackRoot(const QString& module_id) const;
  void pruneRollbacks(const QString& module_id, int keep = 3) const;

  SettingsService& settings_;
  PathService& paths_;
  QVariantList modules_;
};

}  // namespace xenon::launcher
