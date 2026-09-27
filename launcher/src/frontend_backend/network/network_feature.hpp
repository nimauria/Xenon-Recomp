#pragma once

#include "../settings/settings_feature.hpp"

#include <QObject>
#include <QVariantMap>

#include <memory>

namespace xenon::launcher::frontend_backend {

class NetworkFeature final : public QObject {
  Q_OBJECT

 public:
  explicit NetworkFeature(SettingsFeature& settings, QObject* parent = nullptr);
  ~NetworkFeature() override;

  void initialize();
  void reconfigure();
  void refreshHealth();
  void shutdown();
  [[nodiscard]] QVariantMap snapshot() const;

 signals:
  void changed();

 private:
  struct Implementation;
  void publish(QVariantMap snapshot);

  SettingsFeature& settings_;
  std::unique_ptr<Implementation> implementation_;
  QVariantMap snapshot_;
};

}  // namespace xenon::launcher::frontend_backend
