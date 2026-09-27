#pragma once

#include "../../launcher_config.hpp"
#include "../../services/settings_service.hpp"

#include <QObject>

namespace xenon::launcher::frontend_backend {

class ApplicationFeature final : public QObject {
  Q_OBJECT

 public:
  explicit ApplicationFeature(SettingsService& settings, QObject* parent = nullptr);

  [[nodiscard]] bool systemDark() const noexcept;
  [[nodiscard]] bool systemHighContrast() const noexcept;
  [[nodiscard]] int rememberedPage() const;
  [[nodiscard]] int initialPage() const;
  void rememberPage(int page_index);
  [[nodiscard]] bool featureEnabled(const QString& feature) const noexcept;

 signals:
  void systemAppearanceChanged();

 private:
  SettingsService& settings_;
};

}  // namespace xenon::launcher::frontend_backend
