#pragma once

#include "../../../services/service_result.hpp"

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {

class SettingsCatalog final {
 public:
  [[nodiscard]] static QVariantList categories();
  [[nodiscard]] static QVariantMap definition(const QString& key);
  [[nodiscard]] static QVariant defaultValue(const QString& key);
  [[nodiscard]] static QVariantList options(const QString& key);
  [[nodiscard]] static bool contains(const QString& key);
  [[nodiscard]] static QString categoryFor(const QString& key);
  [[nodiscard]] static QStringList keysForCategory(const QString& category_id);
  [[nodiscard]] static ServiceResult normalize(const QString& key, const QVariant& value);
};

}  // namespace xenon::launcher::frontend_backend
