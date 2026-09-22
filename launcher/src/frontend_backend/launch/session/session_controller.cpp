#include "session_controller.hpp"

#include "../launch_feature.hpp"
#include "../../../services/library_service.hpp"
#include "../../../services/preparation_service.hpp"
#include "../../../services/settings_service.hpp"

#include <QDateTime>
#include <QUuid>
#include <QtGlobal>

namespace xenon::launcher::frontend_backend {
namespace {
constexpr auto kHistoryKey = "session/history";
constexpr auto kTestHistoryKey = "session/history-test";
}

SessionController::SessionController(LaunchFeature& launch, LibraryService& library,
                                     SettingsService& storage, PreparationService& preparation,
                                     bool test_mode, QObject* parent)
    : QObject(parent), launch_(launch), library_(library), storage_(storage),
      preparation_(preparation), test_mode_(test_mode) {
  tick_timer_.setInterval(1000);
  tick_timer_.setSingleShot(false);
  connect(&tick_timer_, &QTimer::timeout, this, [this]() {
    if (current_.state != SessionState::Running) return;
    current_.elapsed_ms = runningElapsedMs();
    emit changed();
  });
  restoreHistory();
}

QVariantMap SessionController::currentSession() const {
  auto snapshot = current_;
  if (snapshot.state == SessionState::Running) snapshot.elapsed_ms = runningElapsedMs();
  return snapshot.toVariantMap(true);
}

QVariantList SessionController::history() const { return history_; }
QString SessionController::state() const { return sessionStateId(current_.state); }
bool SessionController::active() const noexcept { return current_.active(); }
bool SessionController::running() const noexcept { return current_.state == SessionState::Running; }

ServiceResult SessionController::start(const QString& game_id) {
  const auto requested_game = game_id.trimmed();
  if (requested_game.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Launch session"),
                                  QStringLiteral("No game was selected."));
  }
  if (current_.active()) {
    return ServiceResult::failure(
        QStringLiteral("Session already active"),
        current_.game_id == requested_game
            ? QStringLiteral("This game already has an active launcher session.")
            : QStringLiteral("Stop the current game session before launching another game."),
        currentSession());
  }

  if (current_.state == SessionState::Failed) {
    current_ = {};
    emit changed();
  }

  ++generation_;
  current_ = {};
  current_.session_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  current_.game_id = requested_game;
  current_.requested_at = QDateTime::currentDateTimeUtc();
  current_.state = SessionState::Preparing;
  emit changed();
  schedulePrepare(generation_);

  return ServiceResult::success(QStringLiteral("Launch requested"),
                                QStringLiteral("Xenon is preparing the game session."),
                                currentSession());
}

ServiceResult SessionController::stop() {
  if (current_.state == SessionState::Idle) {
    return ServiceResult::failure(QStringLiteral("Stop session"),
                                  QStringLiteral("There is no active game session."));
  }
  if (current_.state == SessionState::Failed) {
    dismissFailure();
    return ServiceResult::success(QStringLiteral("Launch failure dismissed"));
  }
  if (current_.state == SessionState::Stopping) {
    return ServiceResult::failure(QStringLiteral("Stop session"),
                                  QStringLiteral("The current session is already stopping."));
  }

  const auto previous = current_.state;
  ++generation_;
  setState(SessionState::Stopping);
  const auto generation = generation_;

  if (previous == SessionState::Preparing || previous == SessionState::Validating ||
      previous == SessionState::Starting) {
    if (previous == SessionState::Preparing && preparation_.isPreparing(current_.game_id)) {
      // Cooperative cancellation (Part 19): the worker terminates its own
      // compiler child process tree and exits on its own; the connected
      // finished() handler above still runs, but the generation check
      // there will already have advanced by the time it fires, so it is a
      // no-op - finishCancellation() below is what actually resolves this
      // pending session.
      preparation_.cancel(current_.game_id);
    }
    QTimer::singleShot(0, this, [this, generation]() { finishCancellation(generation); });
    return ServiceResult::success(QStringLiteral("Launch cancelled"),
                                  QStringLiteral("The pending game launch is being cancelled."));
  }

  QTimer::singleShot(0, this, [this, generation]() { finishStop(generation); });
  return ServiceResult::success(QStringLiteral("Stopping game"),
                                QStringLiteral("Xenon asked the runtime to stop the current session."));
}

void SessionController::dismissFailure() {
  if (current_.state != SessionState::Failed) return;
  ++generation_;
  current_ = {};
  emit changed();
}

void SessionController::clearHistory() {
  history_.clear();
  persistHistory();
  emit historyChanged();
}

void SessionController::setState(SessionState state) {
  if (current_.state == state) return;
  current_.state = state;
  emit changed();
}

void SessionController::schedulePrepare(quint64 generation) {
  QTimer::singleShot(0, this, [this, generation]() { preparePhase(generation); });
}

void SessionController::scheduleValidate(quint64 generation) {
  QTimer::singleShot(0, this, [this, generation]() { validatePhase(generation); });
}

void SessionController::scheduleStart(quint64 generation) {
  QTimer::singleShot(0, this, [this, generation]() { startPhase(generation); });
}

void SessionController::preparePhase(quint64 generation) {
  if (generation != generation_ || current_.state != SessionState::Preparing) return;
  // Must run before configurationFor()/usesAutomaticPreparation() below - a
  // module installed after this game was imported without one needs to be
  // resolved before this same Preparing phase decides whether an automatic
  // native-module build is needed (Part 7).
  launch_.ensureModuleResolved(current_.game_id);
  const auto configuration = launch_.configurationFor(current_.game_id);
  if (configuration.isEmpty()) {
    failCurrent(QStringLiteral("configuration"),
                ServiceResult::failure(QStringLiteral("Launch configuration failed"),
                                       QStringLiteral("Xenon could not assemble a launch configuration for this game.")));
    return;
  }

  current_.configuration = configuration;
  current_.title = configuration.value(QStringLiteral("title")).toString();
  current_.profile_id = configuration.value(QStringLiteral("profileId")).toString();
  current_.profile_name = configuration.value(QStringLiteral("profileName")).toString();
  current_.module_id = configuration.value(QStringLiteral("moduleId")).toString();
  current_.module_name = configuration.value(QStringLiteral("moduleName")).toString();

  // Automatic game preparation (docs/development/GAME_PREPARATION.md, Part 9): a module
  // that relies on Xenon's automatic pipeline (no pre-built native
  // extension of its own) needs its cache checked - and, if stale/missing,
  // a real out-of-process build - before this session can proceed past
  // Preparing. A traditional module (already resolved a native extension
  // path via configurationFor()) skips straight to Validating exactly as
  // before - zero behavior change for it.
  if (test_mode_ || !preparation_.usesAutomaticPreparation(current_.game_id)) {
    setState(SessionState::Validating);
    scheduleValidate(generation);
    return;
  }

  QString cached_path;
  const auto cache_check = preparation_.checkCache(current_.game_id, cached_path);
  if (!cache_check.ok) {
    failCurrent(QStringLiteral("preparation-check"), cache_check);
    return;
  }
  if (!cached_path.isEmpty()) {
    current_.configuration.insert(QStringLiteral("nativeExtensionPath"), cached_path);
    setState(SessionState::Validating);
    scheduleValidate(generation);
    return;
  }

  current_.progress_phase = QStringLiteral("Inspecting");
  current_.progress_message = QStringLiteral("Preparing %1 for its first launch...").arg(current_.title);
  current_.progress_percent = 0;
  emit changed();

  QObject::disconnect(preparation_progress_connection_);
  QObject::disconnect(preparation_finished_connection_);
  const auto game_id = current_.game_id;
  preparation_progress_connection_ = connect(
      &preparation_, &PreparationService::progress, this,
      [this, generation, game_id](const QString& progress_game_id, const QString& phase, int percent,
                                  const QString& task) {
        if (generation != generation_ || progress_game_id != game_id ||
            current_.state != SessionState::Preparing) {
          return;
        }
        current_.progress_phase = phase;
        current_.progress_message = task.isEmpty() ? phase : task;
        current_.progress_percent = percent;
        emit changed();
      });
  preparation_finished_connection_ = connect(
      &preparation_, &PreparationService::finished, this,
      [this, generation, game_id](const QString& finished_game_id, bool success,
                                  const QString& native_extension_path, const QString& error_message) {
        if (finished_game_id != game_id) return;
        QObject::disconnect(preparation_progress_connection_);
        QObject::disconnect(preparation_finished_connection_);
        if (generation != generation_ || current_.state != SessionState::Preparing) return;
        if (!success) {
          failCurrent(QStringLiteral("preparation"),
                      ServiceResult::failure(QStringLiteral("Preparation failed"), error_message));
          return;
        }
        current_.configuration.insert(QStringLiteral("nativeExtensionPath"), native_extension_path);
        current_.progress_percent = 100;
        setState(SessionState::Validating);
        scheduleValidate(generation);
      });
  preparation_.beginPreparation(current_.game_id);
}

void SessionController::validatePhase(quint64 generation) {
  if (generation != generation_ || current_.state != SessionState::Validating) return;
  const auto validation = launch_.validate(current_.game_id);
  if (!validation.ok) {
    failCurrent(QStringLiteral("validation"), validation);
    return;
  }
  setState(SessionState::Starting);
  scheduleStart(generation);
}

void SessionController::startPhase(quint64 generation) {
  if (generation != generation_ || current_.state != SessionState::Starting) return;
  const auto result = launch_.startValidated(current_.game_id);
  if (!result.ok) {
    failCurrent(QStringLiteral("runtime-start"), result);
    return;
  }

  current_.started_at = QDateTime::currentDateTimeUtc();
  current_.elapsed_ms = 0;
  current_.error = {};
  elapsed_.restart();
  tick_timer_.start();
  setState(SessionState::Running);

  const auto behavior = launch_.afterLaunchBehavior();
  if (behavior == QStringLiteral("Minimize launcher")) {
    emit launcherActionRequested(QStringLiteral("minimize"));
  } else if (behavior == QStringLiteral("Close launcher")) {
    emit launcherActionRequested(QStringLiteral("close"));
  }
}

void SessionController::finishCancellation(quint64 generation) {
  if (generation != generation_ || current_.state != SessionState::Stopping) return;
  current_.ended_at = QDateTime::currentDateTimeUtc();
  current_.elapsed_ms = 0;
  archiveCurrent(QStringLiteral("cancelled"));
  current_ = {};
  emit changed();
}

void SessionController::finishStop(quint64 generation) {
  if (generation != generation_ || current_.state != SessionState::Stopping) return;
  const auto result = launch_.stop();
  if (!result.ok) {
    failCurrent(QStringLiteral("runtime-stop"), result);
    return;
  }

  tick_timer_.stop();
  current_.elapsed_ms = runningElapsedMs();
  current_.ended_at = QDateTime::currentDateTimeUtc();
  if (!test_mode_ && !current_.game_id.isEmpty()) {
    (void)library_.recordSessionEnded(current_.game_id, current_.elapsed_ms, QStringLiteral("stopped"));
  }
  archiveCurrent(QStringLiteral("stopped"));
  current_ = {};
  emit changed();
}

void SessionController::failCurrent(const QString& code, const ServiceResult& result) {
  tick_timer_.stop();
  if (current_.started_at.isValid()) current_.elapsed_ms = runningElapsedMs();
  current_.ended_at = QDateTime::currentDateTimeUtc();
  current_.error.code = code;
  current_.error.title = result.title.isEmpty() ? QStringLiteral("Session failed") : result.title;
  current_.error.message = result.message;
  current_.error.details = result.data.toMap();
  if (current_.started_at.isValid() && !test_mode_ && !current_.game_id.isEmpty()) {
    (void)library_.recordSessionEnded(current_.game_id, current_.elapsed_ms, QStringLiteral("failed"));
  }
  current_.state = SessionState::Failed;
  archiveCurrent(QStringLiteral("failed"));
  emit changed();
  emit notificationRequested(current_.error.title, current_.error.message);
}

void SessionController::archiveCurrent(const QString& outcome) {
  current_.outcome = outcome;
  auto archived = current_.toVariantMap(false);
  history_.prepend(archived);
  while (history_.size() > kHistoryLimit) history_.removeLast();
  persistHistory();
  emit historyChanged();
}

void SessionController::restoreHistory() {
  const auto key = QString::fromLatin1(test_mode_ ? kTestHistoryKey : kHistoryKey);
  history_ = storage_.value(key, QVariantList{}).toList();
  while (history_.size() > kHistoryLimit) history_.removeLast();
}

void SessionController::persistHistory() {
  const auto key = QString::fromLatin1(test_mode_ ? kTestHistoryKey : kHistoryKey);
  storage_.setValue(key, history_);
}

qint64 SessionController::runningElapsedMs() const {
  return elapsed_.isValid() ? qMax<qint64>(0, elapsed_.elapsed()) : current_.elapsed_ms;
}

}  // namespace xenon::launcher::frontend_backend
