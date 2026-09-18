#pragma once

#include "discord/discord_presence_feature.hpp"
#include "../filesystem/filesystem_feature.hpp"

#include <QObject>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {

class CommunityFeature final : public QObject {
  Q_OBJECT

 public:
  CommunityFeature(SettingsFeature& settings, SessionController& session,
                   FilesystemFeature& filesystem, QString discord_application_id,
                   QObject* parent = nullptr);

  [[nodiscard]] QVariantMap info() const;
  [[nodiscard]] QVariantMap discordPresenceState() const;
  [[nodiscard]] ServiceResult openDiscord();
  [[nodiscard]] ServiceResult openProjectPage();
  void setPage(const QString& page_name);
  [[nodiscard]] ServiceResult refreshDiscordPresence();

 signals:
  void changed();

 private:
  FilesystemFeature& filesystem_;
  DiscordPresenceFeature discord_presence_;
};

}  // namespace xenon::launcher::frontend_backend
