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
  [[nodiscard]] QVariantList dlcCatalog(const QString& module_id) const;
  [[nodiscard]] ServiceResult refresh();
  [[nodiscard]] ServiceResult setEnabled(const QString& module_id, bool enabled);
  [[nodiscard]] ServiceResult remove(const QString& module_id);
  [[nodiscard]] ServiceResult installFromDirectory(const QString& expected_module_id,
                                                   const QString& source_directory,
                                                   bool replace_existing = true);
  [[nodiscard]] QVariantMap inspectDirectory(const QString& module_dir) const;
  [[nodiscard]] ServiceResult verify(const QString& module_id) const;
  [[nodiscard]] QString modulePath(const QString& module_id) const;
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

  SettingsService& settings_;
  PathService& paths_;
  QVariantList modules_;
};

}  // namespace xenon::launcher
