#pragma once

#include <QDateTime>
#include <QString>
#include <QVariantMap>

namespace xenon::launcher {

enum class SessionState {
  Idle,
  Preparing,
  Validating,
  Starting,
  Running,
  Stopping,
  Failed,
};

[[nodiscard]] QString sessionStateId(SessionState state);
[[nodiscard]] QString sessionStateLabel(SessionState state);

struct SessionError final {
  QString code;
  QString title;
  QString message;
  QVariantMap details;

  [[nodiscard]] bool empty() const noexcept { return title.isEmpty() && message.isEmpty(); }
  [[nodiscard]] QVariantMap toVariantMap() const;
};

struct SessionRecord final {
  QString session_id;
  QString game_id;
  QString title;
  QString profile_id;
  QString profile_name;
  QString module_id;
  QString module_name;
  SessionState state = SessionState::Idle;
  QString outcome;
  QDateTime requested_at;
  QDateTime started_at;
  QDateTime ended_at;
  qint64 elapsed_ms = 0;
  QVariantMap configuration;
  SessionError error;
  // Automatic game preparation (docs/development/GAME_PREPARATION.md) progress, populated
  // only while state == Preparing and this game needed a native module
  // build. progress_percent is -1 when not meaningful (e.g. compiling).
  QString progress_phase;
  QString progress_message;
  int progress_percent = -1;

  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool busy() const noexcept;
  [[nodiscard]] bool canStop() const noexcept;
  [[nodiscard]] bool canCancel() const noexcept;
  [[nodiscard]] QVariantMap toVariantMap(bool include_configuration = false) const;
};

}  // namespace xenon::launcher
