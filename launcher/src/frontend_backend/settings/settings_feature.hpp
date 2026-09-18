#pragma once

#include "../../services/service_result.hpp"
#include "../../services/settings_service.hpp"

#include <QObject>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {

class SettingsFeature final : public QObject {
  Q_OBJECT

 public:
  explicit SettingsFeature(SettingsService& settings, QObject* parent = nullptr);

  [[nodiscard]] QVariant value(const QString& key, const QVariant& fallback = {}) const;
  [[nodiscard]] bool boolValue(const QString& key, bool fallback = false) const;
  [[nodiscard]] QString stringValue(const QString& key, const QString& fallback = {}) const;
  [[nodiscard]] double numberValue(const QString& key, double fallback = 0.0) const;
  [[nodiscard]] int intValue(const QString& key, int fallback = 0) const;

  // Internal writes are used by launcher-owned features after they have already
  // performed domain-specific validation (paths, manifests, migration, etc.).
  void setValue(const QString& key, const QVariant& value);
  void reset(const QString& key);

  // UI-facing settings always pass through the catalog. This is the boundary
  // that prevents QML from inventing unsupported values or defaults.
  [[nodiscard]] ServiceResult setValidatedValue(const QString& key, const QVariant& value);
  [[nodiscard]] ServiceResult resetCategory(const QString& category_id);
  [[nodiscard]] ServiceResult resetAll();

  [[nodiscard]] QVariantList categories() const;
  [[nodiscard]] QVariantMap definition(const QString& key) const;
  [[nodiscard]] QVariant defaultValue(const QString& key) const;
  [[nodiscard]] QVariantList options(const QString& key) const;
  [[nodiscard]] QString categoryFor(const QString& key) const;
  [[nodiscard]] QStringList keysForCategory(const QString& category_id) const;

 signals:
  void changed(const QString& key, const QVariant& value);

 private:
  [[nodiscard]] static QString storageKey(const QString& key);
  [[nodiscard]] static QVariant resolvedFallback(const QString& key, const QVariant& fallback);

  SettingsService& settings_;
};

}  // namespace xenon::launcher::frontend_backend
