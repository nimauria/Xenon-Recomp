#pragma once

#include <QObject>
#include <QSettings>
#include <QVariant>

namespace xenon::launcher {

class SettingsService final : public QObject {
  Q_OBJECT

 public:
  explicit SettingsService(QObject* parent = nullptr);

  [[nodiscard]] QVariant value(const QString& key, const QVariant& fallback = {}) const;
  [[nodiscard]] bool boolValue(const QString& key, bool fallback = false) const;
  [[nodiscard]] QString stringValue(const QString& key, const QString& fallback = {}) const;
  [[nodiscard]] double numberValue(const QString& key, double fallback = 0.0) const;
  [[nodiscard]] int intValue(const QString& key, int fallback = 0) const;

  void setValue(const QString& key, const QVariant& value);
  void remove(const QString& key);
  void sync();

 signals:
  void changed(const QString& key, const QVariant& value);

 private:
  mutable QSettings settings_;
};

}  // namespace xenon::launcher
