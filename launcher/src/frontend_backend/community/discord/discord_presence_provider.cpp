#include "discord_presence_provider.hpp"

#include <utility>

#ifndef XENON_LAUNCHER_HAS_DISCORD_SOCIAL_SDK
#define XENON_LAUNCHER_HAS_DISCORD_SOCIAL_SDK 0
#endif

#if XENON_LAUNCHER_HAS_DISCORD_SOCIAL_SDK
#define DISCORDPP_IMPLEMENTATION
#include <discordpp.h>
#endif

namespace xenon::launcher::frontend_backend {

UnavailableDiscordPresenceProvider::UnavailableDiscordPresenceProvider(QString application_id)
    : application_id_(std::move(application_id)) {}

QString UnavailableDiscordPresenceProvider::status() const {
  if (application_id_.trimmed().isEmpty()) {
    return QStringLiteral("Discord application ID is not configured for this build.");
  }
  return QStringLiteral("Discord application is configured, but the Discord Social SDK is not linked in this build.");
}

ServiceResult UnavailableDiscordPresenceProvider::publish(const QVariantMap&) {
  return ServiceResult::failure(QStringLiteral("Discord Rich Presence unavailable"), status());
}

ServiceResult UnavailableDiscordPresenceProvider::clear() { return ServiceResult::success(); }

#if XENON_LAUNCHER_HAS_DISCORD_SOCIAL_SDK
namespace {
class DiscordSocialSdkPresenceProvider final : public IDiscordPresenceProvider {
 public:
  explicit DiscordSocialSdkPresenceProvider(quint64 application_id) : application_id_(application_id) {
    client_.SetApplicationId(application_id_);
    last_status_ = QStringLiteral("Discord Social SDK ready");
  }

  [[nodiscard]] QString id() const override { return QStringLiteral("discord-social-sdk"); }
  [[nodiscard]] bool available() const noexcept override { return application_id_ != 0; }
  [[nodiscard]] QString status() const override { return last_status_; }

  [[nodiscard]] ServiceResult publish(const QVariantMap& activity_map) override {
    if (!available()) {
      return ServiceResult::failure(QStringLiteral("Discord Rich Presence"),
                                    QStringLiteral("Discord application ID is invalid."));
    }

    discordpp::Activity activity{};
    activity.SetType(discordpp::ActivityTypes::Playing);

    const auto details = activity_map.value(QStringLiteral("details")).toString().trimmed();
    const auto state = activity_map.value(QStringLiteral("state")).toString().trimmed();
    if (!details.isEmpty()) activity.SetDetails(details.toStdString());
    if (!state.isEmpty()) activity.SetState(state.toStdString());

    const auto large_image = activity_map.value(QStringLiteral("largeImage")).toString().trimmed();
    const auto large_text = activity_map.value(QStringLiteral("largeText")).toString().trimmed();
    if (!large_image.isEmpty() || !large_text.isEmpty()) {
      discordpp::ActivityAssets assets{};
      if (!large_image.isEmpty()) assets.SetLargeImage(large_image.toStdString());
      if (!large_text.isEmpty()) assets.SetLargeText(large_text.toStdString());
      activity.SetAssets(std::move(assets));
    }

    const auto start_ms = activity_map.value(QStringLiteral("startTimestampMs")).toLongLong();
    if (start_ms > 0) {
      discordpp::ActivityTimestamps timestamps{};
      timestamps.SetStart(static_cast<uint64_t>(start_ms));
      activity.SetTimestamps(std::move(timestamps));
    }

    const auto buttons = activity_map.value(QStringLiteral("buttons")).toList();
    for (const auto& candidate : buttons) {
      const auto map = candidate.toMap();
      const auto label = map.value(QStringLiteral("label")).toString().trimmed();
      const auto url = map.value(QStringLiteral("url")).toString().trimmed();
      if (label.isEmpty() || url.isEmpty()) continue;
      discordpp::ActivityButton button{};
      button.SetLabel(label.toStdString());
      button.SetUrl(url.toStdString());
      activity.AddButton(std::move(button));
    }

    client_.UpdateRichPresence(std::move(activity), [this](discordpp::ClientResult result) {
      last_status_ = result.Successful()
          ? QStringLiteral("Rich Presence published to Discord")
          : QString::fromStdString(result.ToString());
    });
    return ServiceResult::success(QStringLiteral("Discord Rich Presence"),
                                  QStringLiteral("Presence update queued for the Discord desktop client."));
  }

  [[nodiscard]] ServiceResult clear() override {
    if (!available()) return ServiceResult::success();
    client_.ClearRichPresence();
    last_status_ = QStringLiteral("Rich Presence cleared");
    return ServiceResult::success();
  }

  void pump() override { discordpp::RunCallbacks(); }

 private:
  quint64 application_id_ = 0;
  discordpp::Client client_{};
  QString last_status_;
};
}  // namespace
#endif

std::unique_ptr<IDiscordPresenceProvider> createDiscordPresenceProvider(const QString& application_id) {
#if XENON_LAUNCHER_HAS_DISCORD_SOCIAL_SDK
  bool ok = false;
  const auto id = application_id.trimmed().toULongLong(&ok);
  if (ok && id != 0) return std::make_unique<DiscordSocialSdkPresenceProvider>(id);
#endif
  return std::make_unique<UnavailableDiscordPresenceProvider>(application_id);
}

}  // namespace xenon::launcher::frontend_backend
