#include "settings_service.hpp"

#include <QMetaType>

namespace xenon::launcher {
SettingsService::SettingsService(QObject* parent)
    : QObject(parent), settings_(QStringLiteral("Project Xenon"), QStringLiteral("Xenon Launcher")) {}

QVariant SettingsService::value(const QString& key, const QVariant& fallback) const {
  return settings_.value(key, fallback);
}

bool SettingsService::boolValue(const QString& key, bool fallback) const {
  const auto current = value(key, fallback);
  const auto type = current.metaType().id();
  if (type == QMetaType::Bool) return current.toBool();
  if (type == QMetaType::Int || type == QMetaType::UInt ||
      type == QMetaType::LongLong || type == QMetaType::ULongLong ||
      type == QMetaType::Double || type == QMetaType::Float) {
    return current.toDouble() != 0.0;
  }

  const auto text = current.toString().trimmed().toLower();
  if (text == QStringLiteral("true") || text == QStringLiteral("1") ||
      text == QStringLiteral("yes") || text == QStringLiteral("on")) {
    return true;
  }
  if (text == QStringLiteral("false") || text == QStringLiteral("0") ||
      text == QStringLiteral("no") || text == QStringLiteral("off") || text.isEmpty()) {
    return false;
  }
  return fallback;
}

QString SettingsService::stringValue(const QString& key, const QString& fallback) const {
  const auto current = value(key, fallback);
  return current.isValid() && !current.isNull() ? current.toString() : fallback;
}

double SettingsService::numberValue(const QString& key, double fallback) const {
  bool ok = false;
  const auto number = value(key, fallback).toDouble(&ok);
  return ok ? number : fallback;
}

int SettingsService::intValue(const QString& key, int fallback) const {
  bool ok = false;
  const auto number = value(key, fallback).toInt(&ok);
  return ok ? number : fallback;
}

void SettingsService::setValue(const QString& key, const QVariant& value) {
  if (settings_.value(key) == value) return;
  settings_.setValue(key, value);
  emit changed(key, value);
}

void SettingsService::remove(const QString& key) {
  if (!settings_.contains(key)) return;
  settings_.remove(key);
  emit changed(key, QVariant{});
}

void SettingsService::sync() {
  settings_.sync();
}

}  // namespace xenon::launcher
