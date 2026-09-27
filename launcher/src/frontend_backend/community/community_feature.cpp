#include "community_feature.hpp"

#include <utility>

namespace xenon::launcher::frontend_backend {
namespace {
constexpr auto kDiscordUrl = "https://discord.gg/zVGw3HADgA";
constexpr auto kProjectUrl = "https://github.com/nimauria/Xenon-Recomp";
}

CommunityFeature::CommunityFeature(SettingsFeature& settings, SessionController& session,
                                   FilesystemFeature& filesystem, QString discord_application_id,
                                   bool suppress_presence, QObject* parent)
    : QObject(parent),
      filesystem_(filesystem),
      discord_presence_(settings, session, std::move(discord_application_id), suppress_presence, this) {
  connect(&discord_presence_, &DiscordPresenceFeature::changed, this, &CommunityFeature::changed);
}

QVariantMap CommunityFeature::info() const {
  return {{QStringLiteral("discordInvite"), QString::fromLatin1(kDiscordUrl)},
          {QStringLiteral("projectUrl"), QString::fromLatin1(kProjectUrl)},
          {QStringLiteral("discordLabel"), QStringLiteral("Join the Xenon Discord")},
          {QStringLiteral("supportText"),
           QStringLiteral("Get launcher support, follow development and join the Xenon community on Discord.")}};
}

QVariantMap CommunityFeature::discordPresenceState() const { return discord_presence_.state(); }

ServiceResult CommunityFeature::openDiscord() {
  return filesystem_.openExternalUrl(QString::fromLatin1(kDiscordUrl));
}

ServiceResult CommunityFeature::openProjectPage() {
  return filesystem_.openExternalUrl(QString::fromLatin1(kProjectUrl));
}

void CommunityFeature::setPage(const QString& page_name) { discord_presence_.setPage(page_name); }

ServiceResult CommunityFeature::refreshDiscordPresence() { return discord_presence_.refresh(); }

}  // namespace xenon::launcher::frontend_backend
