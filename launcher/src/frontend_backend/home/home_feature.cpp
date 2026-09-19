#include "home_feature.hpp"

#include "../launch/session/session_controller.hpp"
#include "../library/library_feature.hpp"
#include "../modules/modules_feature.hpp"
#include "../notifications/notification_center_feature.hpp"

#include <QDateTime>
#include <QList>
#include <QtGlobal>

#include <algorithm>
#include <utility>

namespace xenon::launcher::frontend_backend {
namespace {
constexpr int kRecentGameLimit = 6;
constexpr int kRecentSessionLimit = 8;
constexpr int kRecentActivityLimit = 8;

QDateTime parseIso(const QVariant& value) {
  auto parsed = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
  if (!parsed.isValid()) parsed = QDateTime::fromString(value.toString(), Qt::ISODate);
  return parsed;
}

QString relativeTime(const QDateTime& timestamp) {
  if (!timestamp.isValid()) return QStringLiteral("Not recorded");
  const auto local = timestamp.toLocalTime();
  const auto now = QDateTime::currentDateTime();
  auto seconds = local.secsTo(now);
  if (seconds < 0) seconds = 0;
  if (seconds < 60) return QStringLiteral("Just now");
  if (seconds < 3600) return QStringLiteral("%1m ago").arg(seconds / 60);
  if (seconds < 86400) return QStringLiteral("%1h ago").arg(seconds / 3600);
  if (seconds < 604800) return QStringLiteral("%1d ago").arg(seconds / 86400);
  return local.date().toString(QStringLiteral("dd MMM yyyy"));
}

QString outcomeLabel(const QString& outcome) {
  const auto normalized = outcome.trimmed().toLower();
  if (normalized == QStringLiteral("failed")) return QStringLiteral("Failed");
  if (normalized == QStringLiteral("cancelled")) return QStringLiteral("Cancelled");
  if (normalized == QStringLiteral("stopped")) return QStringLiteral("Completed");
  if (normalized == QStringLiteral("running")) return QStringLiteral("Running");
  return normalized.isEmpty() ? QStringLiteral("Session") : normalized;
}

QVariantMap enrichGame(QVariantMap game) {
  const auto last_played = parseIso(game.value(QStringLiteral("lastPlayedAt")));
  game.insert(QStringLiteral("lastPlayedLabel"), relativeTime(last_played));
  return game;
}

QVariantMap enrichSession(QVariantMap session) {
  auto when = parseIso(session.value(QStringLiteral("endedAt")));
  if (!when.isValid()) when = parseIso(session.value(QStringLiteral("startedAt")));
  if (!when.isValid()) when = parseIso(session.value(QStringLiteral("requestedAt")));
  const auto outcome = session.value(QStringLiteral("outcome")).toString();
  session.insert(QStringLiteral("whenLabel"), relativeTime(when));
  session.insert(QStringLiteral("outcomeLabel"), outcomeLabel(outcome));
  session.insert(QStringLiteral("failed"), outcome.compare(QStringLiteral("failed"), Qt::CaseInsensitive) == 0);
  return session;
}

QVariantMap enrichActivity(QVariantMap activity) {
  activity.insert(QStringLiteral("whenLabel"), relativeTime(parseIso(activity.value(QStringLiteral("timestamp")))));
  return activity;
}
}  // namespace

HomeFeature::HomeFeature(LibraryFeature& library, ModulesFeature& modules,
                         SessionController& session, NotificationCenterFeature& notifications,
                         QObject* parent)
    : QObject(parent),
      library_(library),
      modules_(modules),
      session_(session),
      notifications_(notifications) {
  connect(&library_, &LibraryFeature::changed, this, &HomeFeature::changed);
  connect(&modules_, &ModulesFeature::changed, this, &HomeFeature::changed);
  connect(&modules_, &ModulesFeature::updateStateChanged, this,
          [this](const QString&) { emit changed(); });
  connect(&session_, &SessionController::changed, this, &HomeFeature::changed);
  connect(&session_, &SessionController::historyChanged, this, &HomeFeature::changed);
  connect(&notifications_, &NotificationCenterFeature::changed, this, &HomeFeature::changed);
}

QVariantMap HomeFeature::snapshot() const {
  const auto library_entries = library_.entries();
  const auto module_entries = modules_.entries();
  const auto session_history = session_.history();
  const auto notification_entries = notifications_.entries();

  qint64 total_play_time_ms = 0;
  qint64 total_play_count = 0;
  int enabled_modules = 0;
  int module_updates = 0;
  int failed_sessions = 0;

  QList<QVariantMap> played_games;
  played_games.reserve(library_entries.size());
  for (const auto& value : library_entries) {
    auto game = value.toMap();
    total_play_time_ms += qMax<qint64>(0, game.value(QStringLiteral("totalPlayTimeMs")).toLongLong());
    total_play_count += qMax<qint64>(0, game.value(QStringLiteral("playCount")).toLongLong());
    const auto played_at = parseIso(game.value(QStringLiteral("lastPlayedAt")));
    if (played_at.isValid() || game.value(QStringLiteral("playCount")).toLongLong() > 0) {
      played_games.push_back(enrichGame(std::move(game)));
    }
  }

  std::stable_sort(played_games.begin(), played_games.end(), [](const QVariantMap& left, const QVariantMap& right) {
    return parseIso(left.value(QStringLiteral("lastPlayedAt"))) >
           parseIso(right.value(QStringLiteral("lastPlayedAt")));
  });

  for (const auto& value : module_entries) {
    const auto module = value.toMap();
    if (module.value(QStringLiteral("active")).toBool()) ++enabled_modules;
    if (module.value(QStringLiteral("updateAvailable")).toBool()) ++module_updates;
  }

  QVariantList recent_sessions;
  recent_sessions.reserve(qMin(kRecentSessionLimit, static_cast<int>(session_history.size())));
  for (int i = 0; i < session_history.size() && i < kRecentSessionLimit; ++i) {
    auto session = enrichSession(session_history.at(i).toMap());
    if (session.value(QStringLiteral("failed")).toBool()) ++failed_sessions;
    recent_sessions.append(session);
  }
  for (int i = kRecentSessionLimit; i < session_history.size(); ++i) {
    if (session_history.at(i).toMap().value(QStringLiteral("outcome")).toString() == QStringLiteral("failed")) {
      ++failed_sessions;
    }
  }

  QVariantList recent_games;
  recent_games.reserve(qMin(kRecentGameLimit, static_cast<int>(played_games.size())));
  for (int i = 0; i < played_games.size() && i < kRecentGameLimit; ++i) recent_games.append(played_games.at(i));

  QVariantList recent_activity;
  recent_activity.reserve(qMin(kRecentActivityLimit, static_cast<int>(notification_entries.size())));
  for (int i = 0; i < notification_entries.size() && i < kRecentActivityLimit; ++i) {
    recent_activity.append(enrichActivity(notification_entries.at(i).toMap()));
  }

  auto current_session = session_.currentSession();
  QVariantMap continue_game;
  const auto current_game_id = current_session.value(QStringLiteral("gameId")).toString();
  if (!current_game_id.isEmpty()) {
    for (const auto& value : library_entries) {
      const auto game = value.toMap();
      if (game.value(QStringLiteral("gameId")).toString() == current_game_id) {
        continue_game = enrichGame(game);
        break;
      }
    }
  }
  if (continue_game.isEmpty() && !played_games.isEmpty()) continue_game = played_games.constFirst();

  QVariantMap stats{
      {QStringLiteral("games"), library_entries.size()},
      {QStringLiteral("modules"), module_entries.size()},
      {QStringLiteral("enabledModules"), enabled_modules},
      {QStringLiteral("moduleUpdates"), module_updates},
      {QStringLiteral("totalPlayTimeMs"), total_play_time_ms},
      {QStringLiteral("totalPlayCount"), total_play_count},
      {QStringLiteral("sessionCount"), session_history.size()},
      {QStringLiteral("failedSessions"), failed_sessions},
      {QStringLiteral("unreadNotifications"), notifications_.unreadCount()},
  };

  return {
      {QStringLiteral("stats"), stats},
      {QStringLiteral("currentSession"), current_session},
      {QStringLiteral("continueGame"), continue_game},
      {QStringLiteral("recentGames"), recent_games},
      {QStringLiteral("recentSessions"), recent_sessions},
      {QStringLiteral("recentActivity"), recent_activity},
  };
}

}  // namespace xenon::launcher::frontend_backend
