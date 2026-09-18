#include "session_record.hpp"

namespace xenon::launcher {

QString sessionStateId(SessionState state) {
  switch (state) {
    case SessionState::Preparing: return QStringLiteral("preparing");
    case SessionState::Validating: return QStringLiteral("validating");
    case SessionState::Starting: return QStringLiteral("starting");
    case SessionState::Running: return QStringLiteral("running");
    case SessionState::Stopping: return QStringLiteral("stopping");
    case SessionState::Failed: return QStringLiteral("failed");
    case SessionState::Idle:
    default: return QStringLiteral("idle");
  }
}

QString sessionStateLabel(SessionState state) {
  switch (state) {
    case SessionState::Preparing: return QStringLiteral("Preparing");
    case SessionState::Validating: return QStringLiteral("Validating");
    case SessionState::Starting: return QStringLiteral("Starting");
    case SessionState::Running: return QStringLiteral("Running");
    case SessionState::Stopping: return QStringLiteral("Stopping");
    case SessionState::Failed: return QStringLiteral("Launch failed");
    case SessionState::Idle:
    default: return QStringLiteral("Idle");
  }
}

QVariantMap SessionError::toVariantMap() const {
  return {{QStringLiteral("code"), code},
          {QStringLiteral("title"), title},
          {QStringLiteral("message"), message},
          {QStringLiteral("details"), details}};
}

bool SessionRecord::active() const noexcept {
  return state != SessionState::Idle && state != SessionState::Failed;
}

bool SessionRecord::busy() const noexcept {
  return state == SessionState::Preparing || state == SessionState::Validating ||
         state == SessionState::Starting || state == SessionState::Stopping;
}

bool SessionRecord::canStop() const noexcept { return state == SessionState::Running; }

bool SessionRecord::canCancel() const noexcept {
  return state == SessionState::Preparing || state == SessionState::Validating ||
         state == SessionState::Starting;
}

QVariantMap SessionRecord::toVariantMap(bool include_configuration) const {
  QVariantMap result{{QStringLiteral("sessionId"), session_id},
                     {QStringLiteral("gameId"), game_id},
                     {QStringLiteral("title"), title},
                     {QStringLiteral("profileId"), profile_id},
                     {QStringLiteral("profileName"), profile_name},
                     {QStringLiteral("moduleId"), module_id},
                     {QStringLiteral("moduleName"), module_name},
                     {QStringLiteral("state"), sessionStateId(state)},
                     {QStringLiteral("stateLabel"), sessionStateLabel(state)},
                     {QStringLiteral("outcome"), outcome},
                     {QStringLiteral("requestedAt"), requested_at.isValid() ? requested_at.toString(Qt::ISODateWithMs) : QString{}},
                     {QStringLiteral("startedAt"), started_at.isValid() ? started_at.toString(Qt::ISODateWithMs) : QString{}},
                     {QStringLiteral("endedAt"), ended_at.isValid() ? ended_at.toString(Qt::ISODateWithMs) : QString{}},
                     {QStringLiteral("elapsedMs"), elapsed_ms},
                     {QStringLiteral("active"), active()},
                     {QStringLiteral("busy"), busy()},
                     {QStringLiteral("canStop"), canStop()},
                     {QStringLiteral("canCancel"), canCancel()},
                     {QStringLiteral("failed"), state == SessionState::Failed},
                     {QStringLiteral("error"), error.toVariantMap()}};
  if (include_configuration) result.insert(QStringLiteral("configuration"), configuration);
  return result;
}

}  // namespace xenon::launcher
