#pragma once

#include "discord_presence_provider.hpp"

#include "../../launch/session/session_controller.hpp"
#include "../../settings/settings_feature.hpp"

#include <QObject>
#include <QTimer>
#include <QVariantMap>

#include <memory>

namespace xenon::launcher::frontend_backend {

class DiscordPresenceFeature final : public QObject {
  Q_OBJECT

 public:
  DiscordPresenceFeature(SettingsFeature& settings, SessionController& session,
                         QString application_id, bool suppressed = false,
                         QObject* parent = nullptr);
  ~DiscordPresenceFeature() override;

  [[nodiscard]] QVariantMap state() const;
  [[nodiscard]] QVariantMap desiredActivity() const;
  [[nodiscard]] bool requestedEnabled() const;
  [[nodiscard]] bool enabled() const;
  [[nodiscard]] bool showGameTitle() const;
  void setPage(const QString& page_name);
  [[nodiscard]] ServiceResult refresh();
  [[nodiscard]] ServiceResult clear();

 signals:
  void changed();

 private:
  void rebuild();
  [[nodiscard]] QVariantMap buildActivity() const;
  [[nodiscard]] QString pageDetails() const;
  [[nodiscard]] QString sessionDetails(const QVariantMap& session) const;

  SettingsFeature& settings_;
  SessionController& session_;
  std::unique_ptr<IDiscordPresenceProvider> provider_;
  QString application_id_;
  bool suppressed_ = false;
  QString page_ = QStringLiteral("Library");
  QVariantMap desired_activity_;
  QString last_publish_error_;
  QString provider_status_cache_;
  bool last_enabled_ = false;
  QTimer callback_timer_;
};

}  // namespace xenon::launcher::frontend_backend
