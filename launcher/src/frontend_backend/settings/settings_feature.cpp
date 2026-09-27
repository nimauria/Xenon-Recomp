#include "settings_feature.hpp"

#include "schema/settings_catalog.hpp"

namespace xenon::launcher::frontend_backend {

SettingsFeature::SettingsFeature(SettingsService& settings, QObject* parent)
    : QObject(parent), settings_(settings) {
  connect(&settings_, &SettingsService::changed, this,
          [this](const QString& key, const QVariant& value) {
            const auto prefix = QStringLiteral("frontend/");
            if (key.startsWith(prefix)) {
              // QML and the rest of FrontendBackend use public setting keys
              // without the internal QSettings namespace. Do not use sizeof()
              // here: an auto string literal decays to a pointer and sizeof()
              // would strip the pointer width rather than the prefix length.
              emit changed(key.mid(prefix.size()), value);
            }
          });
}

QString SettingsFeature::storageKey(const QString& key) {
  return QStringLiteral("frontend/") + key.trimmed();
}

QVariant SettingsFeature::resolvedFallback(const QString& key, const QVariant& fallback) {
  const auto catalog_default = SettingsCatalog::defaultValue(key);
  if (catalog_default.isValid() && !catalog_default.isNull()) return catalog_default;
  return fallback;
}

QVariant SettingsFeature::value(const QString& key, const QVariant& fallback) const {
  return settings_.value(storageKey(key), resolvedFallback(key, fallback));
}

bool SettingsFeature::boolValue(const QString& key, bool fallback) const {
  return settings_.boolValue(storageKey(key), resolvedFallback(key, fallback).toBool());
}

QString SettingsFeature::stringValue(const QString& key, const QString& fallback) const {
  return settings_.stringValue(storageKey(key), resolvedFallback(key, fallback).toString());
}

double SettingsFeature::numberValue(const QString& key, double fallback) const {
  return settings_.numberValue(storageKey(key), resolvedFallback(key, fallback).toDouble());
}

int SettingsFeature::intValue(const QString& key, int fallback) const {
  return settings_.intValue(storageKey(key), resolvedFallback(key, fallback).toInt());
}

void SettingsFeature::setValue(const QString& key, const QVariant& value) {
  settings_.setValue(storageKey(key), value);
}

void SettingsFeature::reset(const QString& key) {
  settings_.remove(storageKey(key));
}

ServiceResult SettingsFeature::setValidatedValue(const QString& key, const QVariant& value) {
  const auto normalized_key = key.trimmed();
  const auto normalized = SettingsCatalog::normalize(normalized_key, value);
  if (!normalized.ok) return normalized;
  setValue(normalized_key, normalized.data);
  return ServiceResult::success({}, {}, normalized.data);
}

ServiceResult SettingsFeature::resetCategory(const QString& category_id) {
  const auto keys = SettingsCatalog::keysForCategory(category_id);
  if (keys.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Unknown settings category"),
                                  QStringLiteral("The requested settings category is not registered."));
  }
  for (const auto& key : keys) reset(key);
  return ServiceResult::success(QStringLiteral("Settings reset"),
                                QStringLiteral("The %1 settings were restored to their defaults.").arg(category_id));
}

ServiceResult SettingsFeature::resetAll() {
  for (const auto& category : SettingsCatalog::categories()) {
    const auto id = category.toMap().value(QStringLiteral("id")).toString();
    for (const auto& key : SettingsCatalog::keysForCategory(id)) reset(key);
  }
  return ServiceResult::success(QStringLiteral("Settings reset"),
                                QStringLiteral("Launcher settings were restored to their registered defaults."));
}

QVariantList SettingsFeature::categories() const { return SettingsCatalog::categories(); }
QVariantMap SettingsFeature::definition(const QString& key) const { return SettingsCatalog::definition(key); }
QVariant SettingsFeature::defaultValue(const QString& key) const { return SettingsCatalog::defaultValue(key); }
QVariantList SettingsFeature::options(const QString& key) const { return SettingsCatalog::options(key); }
QString SettingsFeature::categoryFor(const QString& key) const { return SettingsCatalog::categoryFor(key); }
QStringList SettingsFeature::keysForCategory(const QString& category_id) const {
  return SettingsCatalog::keysForCategory(category_id);
}

}  // namespace xenon::launcher::frontend_backend
