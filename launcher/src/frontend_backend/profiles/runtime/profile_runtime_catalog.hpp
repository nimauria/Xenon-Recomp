#pragma once

#include "../../../services/service_result.hpp"

#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {
class RuntimeFeature;
class SettingsFeature;

class ProfileRuntimeCatalog final {
 public:
  [[nodiscard]] static QStringList keys();
  [[nodiscard]] static QVariantList definitions(const SettingsFeature& settings,
                                                const RuntimeFeature& runtime,
                                                bool test_mode);
  [[nodiscard]] static QVariantMap defaults(const SettingsFeature& settings);
  [[nodiscard]] static QVariantMap resolved(const QVariantMap& profile,
                                            const SettingsFeature& settings);
  [[nodiscard]] static ServiceResult normalize(const QVariantMap& values,
                                               const RuntimeFeature& runtime,
                                               bool test_mode);
};

}  // namespace xenon::launcher::frontend_backend
