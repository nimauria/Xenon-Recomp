#include "discord_presence_feature.hpp"

#include <utility>

#include <QDateTime>

namespace xenon::launcher::frontend_backend {
namespace {
constexpr auto kCommunityUrl = "https://discord.gg/zVGw3HADgA";
constexpr auto kProjectUrl = "https://github.com/nimauria/Xenon-Recomp";

QVariantMap button(const QString& label, const QString& url) {
  return {{QStringLiteral("label"), label}, {QStringLiteral("url"), url}};
}
}

DiscordPresenceFeature::DiscordPresenceFeature(SettingsFeature& settings, SessionController& session,
                                               QString application_id, bool suppressed, QObject* parent)
    : QObject(parent),
      settings_(settings),
      session_(session),
      provider_(suppressed
                    ? std::make_unique<UnavailableDiscordPresenceProvider>(application_id)
                    : createDiscordPresenceProvider(application_id)),
      application_id_(std::move(application_id)),
      suppressed_(suppressed) {
  // A dormant/unavailable provider must never leave behind a latent opt-in.
  // This keeps future SDK-enabled builds privacy-safe: Rich Presence still
  // requires an explicit user opt-in after the provider actually exists.
  if (!provider_->available() &&
      settings_.boolValue(QStringLiteral("community/discordRichPresence"), false)) {
    settings_.setValue(QStringLiteral("community/discordRichPresence"), false);
  }

  connect(&session_, &SessionController::changed, this, [this]() { rebuild(); });
  connect(&settings_, &SettingsFeature::changed, this,
          [this](const QString& key, const QVariant&) {
            if (key == QStringLiteral("community/discordRichPresence") ||
                key == QStringLiteral("community/discordShowGameTitle")) {
              rebuild();
            }
          });
  provider_status_cache_ = provider_->status();
  callback_timer_.setInterval(500);
  callback_timer_.setSingleShot(false);
  connect(&callback_timer_, &QTimer::timeout, this, [this]() {
    provider_->pump();
    const auto status = provider_->status();
    if (status != provider_status_cache_) {
      provider_status_cache_ = status;
      emit changed();
    }
  });
  if (provider_->available() && !suppressed_) callback_timer_.start();
  rebuild();
}

DiscordPresenceFeature::~DiscordPresenceFeature() {
  callback_timer_.stop();
  (void)provider_->clear();
  provider_->pump();
}

bool DiscordPresenceFeature::requestedEnabled() const {
  return settings_.boolValue(QStringLiteral("community/discordRichPresence"), false);
}

bool DiscordPresenceFeature::enabled() const {
  return !suppressed_ && provider_->available() && requestedEnabled();
}

bool DiscordPresenceFeature::showGameTitle() const {
  return settings_.boolValue(QStringLiteral("community/discordShowGameTitle"), true);
}

void DiscordPresenceFeature::setPage(const QString& page_name) {
  const auto normalized = page_name.trimmed();
  if (normalized.isEmpty() || normalized == page_) return;
  page_ = normalized;
  rebuild();
}

QVariantMap DiscordPresenceFeature::state() const {
  return {{QStringLiteral("enabled"), enabled()},
          {QStringLiteral("requestedEnabled"), requestedEnabled()},
          {QStringLiteral("suppressed"), suppressed_},
          {QStringLiteral("providerId"), provider_->id()},
          {QStringLiteral("providerAvailable"), provider_->available()},
          {QStringLiteral("providerStatus"), provider_->status()},
          {QStringLiteral("applicationIdConfigured"), !application_id_.trimmed().isEmpty()},
          {QStringLiteral("applicationId"), application_id_},
          {QStringLiteral("showGameTitle"), showGameTitle()},
          {QStringLiteral("page"), page_},
          {QStringLiteral("activity"), desired_activity_},
          {QStringLiteral("lastPublishError"), last_publish_error_}};
}

QVariantMap DiscordPresenceFeature::desiredActivity() const { return desired_activity_; }

ServiceResult DiscordPresenceFeature::refresh() {
  desired_activity_ = buildActivity();
  if (!enabled()) {
    last_publish_error_.clear();
    (void)provider_->clear();
    emit changed();
    return ServiceResult::success(QStringLiteral("Discord Rich Presence disabled"));
  }
  if (!provider_->available()) {
    last_publish_error_.clear();
    emit changed();
    return ServiceResult::success(QStringLiteral("Discord Rich Presence unavailable"), provider_->status());
  }
  const auto result = provider_->publish(desired_activity_);
  last_publish_error_ = result.ok ? QString{} : result.message;
  emit changed();
  return result;
}

ServiceResult DiscordPresenceFeature::clear() {
  desired_activity_.clear();
  last_publish_error_.clear();
  const auto result = provider_->clear();
  emit changed();
  return result;
}

void DiscordPresenceFeature::rebuild() {
  const auto next = buildActivity();
  const auto now_enabled = enabled();
  const auto changed_activity = next != desired_activity_;
  const auto changed_enabled = now_enabled != last_enabled_;
  desired_activity_ = next;
  last_enabled_ = now_enabled;
  if (!changed_activity && !changed_enabled) {
    emit changed();
    return;
  }
  if (!now_enabled) {
    last_publish_error_.clear();
    (void)provider_->clear();
    emit changed();
    return;
  }
  if (!provider_->available()) {
    last_publish_error_.clear();
    emit changed();
    return;
  }
  const auto result = provider_->publish(desired_activity_);
  last_publish_error_ = result.ok ? QString{} : result.message;
  emit changed();
}

QString DiscordPresenceFeature::pageDetails() const {
  if (page_ == QStringLiteral("Home")) return QStringLiteral("Viewing Xenon activity");
  if (page_ == QStringLiteral("Modules")) return QStringLiteral("Managing Xenon modules");
  if (page_ == QStringLiteral("Profiles")) return QStringLiteral("Managing launcher profiles");
  if (page_ == QStringLiteral("Settings")) return QStringLiteral("Customising Xenon");
  return QStringLiteral("Browsing the game library");
}

QString DiscordPresenceFeature::sessionDetails(const QVariantMap& session) const {
  const auto title = showGameTitle() ? session.value(QStringLiteral("title")).toString().trimmed() : QString{};
  const auto subject = title.isEmpty() ? QStringLiteral("a game through Xenon") : title;
  const auto state = session.value(QStringLiteral("state")).toString();
  if (state == QStringLiteral("preparing") || state == QStringLiteral("validating") ||
      state == QStringLiteral("starting")) {
    return QStringLiteral("Launching %1").arg(subject);
  }
  if (state == QStringLiteral("running")) return QStringLiteral("Playing %1").arg(subject);
  if (state == QStringLiteral("stopping")) return QStringLiteral("Stopping %1").arg(subject);
  return pageDetails();
}

QVariantMap DiscordPresenceFeature::buildActivity() const {
  const auto session = session_.currentSession();
  const auto active = session.value(QStringLiteral("active")).toBool();
  const auto state = session.value(QStringLiteral("state")).toString();
  const auto use_session = active && state != QStringLiteral("failed") && state != QStringLiteral("idle");

  QVariantMap activity;
  activity.insert(QStringLiteral("type"), QStringLiteral("playing"));
  activity.insert(QStringLiteral("details"), use_session ? sessionDetails(session) : pageDetails());
  activity.insert(QStringLiteral("state"), QStringLiteral("Powered by Xenon"));
  activity.insert(QStringLiteral("buttons"),
                  QVariantList{button(QStringLiteral("Join Xenon Discord"), QString::fromLatin1(kCommunityUrl)),
                               button(QStringLiteral("Project Xenon"), QString::fromLatin1(kProjectUrl))});
  if (use_session && state == QStringLiteral("running")) {
    const auto started = QDateTime::fromString(session.value(QStringLiteral("startedAt")).toString(), Qt::ISODateWithMs);
    if (started.isValid()) activity.insert(QStringLiteral("startTimestampMs"), started.toMSecsSinceEpoch());
  }
  return activity;
}

}  // namespace xenon::launcher::frontend_backend
