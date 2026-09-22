#pragma once

#include "../../../models/session_record.hpp"
#include "../../../services/service_result.hpp"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>
#include <QTimer>
#include <QVariantList>

namespace xenon::launcher {
class LibraryService;
class PreparationService;
class SettingsService;
}

namespace xenon::launcher::frontend_backend {
class LaunchFeature;

class SessionController final : public QObject {
  Q_OBJECT

 public:
  SessionController(LaunchFeature& launch, LibraryService& library, SettingsService& storage,
                    PreparationService& preparation, bool test_mode, QObject* parent = nullptr);

  [[nodiscard]] QVariantMap currentSession() const;
  [[nodiscard]] QVariantList history() const;
  [[nodiscard]] QString state() const;
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool running() const noexcept;

  [[nodiscard]] ServiceResult start(const QString& game_id);
  [[nodiscard]] ServiceResult stop();
  void dismissFailure();
  void clearHistory();

 signals:
  void changed();
  void historyChanged();
  void notificationRequested(const QString& title, const QString& message);
  // Emitted once a session reaches Running, carrying the user's
  // "runtime/afterLaunch" preference ("minimize" or "close"; "keep-open"
  // never emits since there is nothing for the window to do). LauncherBridge
  // turns this into an actual window action - SessionController has no
  // window/application handle of its own.
  void launcherActionRequested(const QString& action);

 private:
  void setState(SessionState state);
  void schedulePrepare(quint64 generation);
  void scheduleValidate(quint64 generation);
  void scheduleStart(quint64 generation);
  void preparePhase(quint64 generation);
  void validatePhase(quint64 generation);
  void startPhase(quint64 generation);
  void finishCancellation(quint64 generation);
  void finishStop(quint64 generation);
  void failCurrent(const QString& code, const ServiceResult& result);
  void archiveCurrent(const QString& outcome);
  void restoreHistory();
  void persistHistory();
  [[nodiscard]] qint64 runningElapsedMs() const;

  LaunchFeature& launch_;
  LibraryService& library_;
  SettingsService& storage_;
  PreparationService& preparation_;
  bool test_mode_ = false;
  SessionRecord current_;
  QVariantList history_;
  QElapsedTimer elapsed_;
  QTimer tick_timer_;
  quint64 generation_ = 0;
  QMetaObject::Connection preparation_progress_connection_;
  QMetaObject::Connection preparation_finished_connection_;
  static constexpr int kHistoryLimit = 50;
};

}  // namespace xenon::launcher::frontend_backend
