#include "recovery_service.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QtGlobal>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#endif

namespace xenon::launcher {
namespace {
constexpr auto kRunMarkerName = "xenon-launcher-running.json";
constexpr auto kRecoveryStateName = "recovery-state.json";
constexpr auto kStartupLogName = "xenon-launcher-startup.log";
constexpr int kAutomaticSafeModeThreshold = 2;
constexpr int kRetainedRecoveryLogs = 5;

QString appDataRoot() {
  auto root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (root.trimmed().isEmpty()) root = QDir::tempPath();
  QDir{}.mkpath(root);
  return root;
}

QString recoveryRoot() {
  const auto root = QDir{appDataRoot()}.filePath(QStringLiteral("recovery"));
  QDir{}.mkpath(root);
  return root;
}

QString markerPath() { return QDir{appDataRoot()}.filePath(QString::fromLatin1(kRunMarkerName)); }
QString statePath() { return QDir{recoveryRoot()}.filePath(QString::fromLatin1(kRecoveryStateName)); }
QString startupLogPath() { return QDir{appDataRoot()}.filePath(QString::fromLatin1(kStartupLogName)); }

bool processAlive(qlonglong pid) {
  if (pid <= 0) return false;
#if defined(Q_OS_WIN)
  const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (handle == nullptr) return false;
  DWORD exit_code = 0;
  const auto alive = GetExitCodeProcess(handle, &exit_code) != FALSE && exit_code == STILL_ACTIVE;
  CloseHandle(handle);
  return alive;
#elif defined(Q_OS_UNIX)
  if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno == EPERM;
#else
  return false;
#endif
}

QVariantMap readJsonMap(const QString& path) {
  QFile file{path};
  if (!file.open(QIODevice::ReadOnly)) return {};
  const auto document = QJsonDocument::fromJson(file.readAll());
  return document.isObject() ? document.object().toVariantMap() : QVariantMap{};
}

bool writeJsonMap(const QString& path, const QVariantMap& value) {
  QDir{}.mkpath(QFileInfo{path}.absolutePath());
  QSaveFile file{path};
  if (!file.open(QIODevice::WriteOnly)) return false;
  const auto data = QJsonDocument{QJsonObject::fromVariantMap(value)}.toJson(QJsonDocument::Indented);
  if (file.write(data) != data.size()) return false;
  return file.commit();
}

QString archiveStartupLog() {
  const QFileInfo source{startupLogPath()};
  if (!source.exists() || !source.isFile() || source.size() <= 0) return {};

  const auto stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
  const auto target = QDir{recoveryRoot()}.filePath(
      QStringLiteral("xenon-launcher-unclean-%1.log").arg(stamp));
  if (!QFile::copy(source.absoluteFilePath(), target)) return {};

  QDir directory{recoveryRoot()};
  const auto logs = directory.entryInfoList(
      {QStringLiteral("xenon-launcher-unclean-*.log")},
      QDir::Files, QDir::Time | QDir::Reversed);
  const auto remove_count = qMax<qsizetype>(0, logs.size() - qsizetype{kRetainedRecoveryLogs});
  for (qsizetype index = 0; index < remove_count; ++index) QFile::remove(logs.at(index).absoluteFilePath());
  return target;
}

QVariantMap currentMarker(bool safe_mode, const QString& phase) {
  return QVariantMap{
      {QStringLiteral("runId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
      {QStringLiteral("startedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {QStringLiteral("phase"), phase},
      {QStringLiteral("safeMode"), safe_mode},
      {QStringLiteral("pid"), static_cast<qlonglong>(QCoreApplication::applicationPid())}};
}

void createCurrentMarker(bool safe_mode, const QString& phase = QStringLiteral("bootstrap")) {
  (void)writeJsonMap(markerPath(), currentMarker(safe_mode, phase));
}

QStringList restartArguments(bool safe_mode) {
  auto arguments = QCoreApplication::arguments();
  if (!arguments.isEmpty()) arguments.removeFirst();
  arguments.removeAll(QStringLiteral("--safe-mode"));
  if (safe_mode) arguments.append(QStringLiteral("--safe-mode"));
  return arguments;
}
}  // namespace

RecoveryService::RecoveryService(QObject* parent) : QObject(parent) {}

void RecoveryService::bootstrap(const QStringList& arguments) {
  auto state = readJsonMap(statePath());
  const auto previous_marker = readJsonMap(markerPath());
  const auto marker_exists = QFileInfo::exists(markerPath());
  const auto previous_pid = previous_marker.value(QStringLiteral("pid"), qlonglong{-1}).toLongLong();
  const auto previous_process_active = marker_exists &&
      previous_pid != static_cast<qlonglong>(QCoreApplication::applicationPid()) && processAlive(previous_pid);
  const auto previous_unclean = marker_exists && !previous_process_active;
  auto consecutive = state.value(QStringLiteral("consecutiveUncleanStarts"), 0).toInt();

  state.insert(QStringLiteral("parallelInstanceDetected"), previous_process_active);
  QString archived_log;
  if (previous_unclean) {
    ++consecutive;
    archived_log = archiveStartupLog();
    state.insert(QStringLiteral("pendingIncident"), true);
    state.insert(QStringLiteral("previousUncleanShutdown"), true);
    state.insert(QStringLiteral("lastIncidentAt"),
                 QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    state.insert(QStringLiteral("previousPhase"),
                 previous_marker.value(QStringLiteral("phase"), QStringLiteral("unknown")));
    state.insert(QStringLiteral("previousStartedAt"), previous_marker.value(QStringLiteral("startedAt")));
    state.insert(QStringLiteral("previousSafeMode"), previous_marker.value(QStringLiteral("safeMode"), false));
    if (!archived_log.isEmpty()) state.insert(QStringLiteral("lastRecoveryLogPath"), archived_log);
  } else {
    state.insert(QStringLiteral("previousUncleanShutdown"), false);
  }

  const auto explicit_safe_mode = arguments.contains(QStringLiteral("--safe-mode"));
  const auto previous_phase = previous_marker.value(QStringLiteral("phase"), QStringLiteral("unknown")).toString();
  const auto failed_during_startup = previous_unclean && previous_phase != QStringLiteral("interactive");
  const auto repeated_unclean = previous_unclean && consecutive >= kAutomaticSafeModeThreshold;
  const auto automatic_safe_mode = failed_during_startup || repeated_unclean;
  const auto safe_mode = explicit_safe_mode || automatic_safe_mode;

  state.insert(QStringLiteral("consecutiveUncleanStarts"), consecutive);
  state.insert(QStringLiteral("currentSafeMode"), safe_mode);
  state.insert(QStringLiteral("currentPid"), static_cast<qlonglong>(QCoreApplication::applicationPid()));
  state.insert(QStringLiteral("automaticSafeMode"), automatic_safe_mode && !explicit_safe_mode);
  state.insert(QStringLiteral("automaticSafeModeReason"),
               !automatic_safe_mode ? QString{}
                                    : failed_during_startup ? QStringLiteral("startup-failure")
                                                            : QStringLiteral("repeated-unclean"));
  state.insert(QStringLiteral("recoveryDirectory"), recoveryRoot());
  state.insert(QStringLiteral("currentPhase"), QStringLiteral("bootstrap"));
  (void)writeJsonMap(statePath(), state);
  createCurrentMarker(safe_mode);
}

void RecoveryService::updateBootstrapPhase(const QString& phase) {
  auto marker = readJsonMap(markerPath());
  if (marker.isEmpty()) return;
  marker.insert(QStringLiteral("phase"), phase.trimmed().isEmpty() ? QStringLiteral("unknown") : phase.trimmed());
  marker.insert(QStringLiteral("phaseUpdatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  (void)writeJsonMap(markerPath(), marker);

  auto state = readJsonMap(statePath());
  state.insert(QStringLiteral("currentPhase"), marker.value(QStringLiteral("phase")));
  (void)writeJsonMap(statePath(), state);
}

void RecoveryService::markProcessCleanExit() {
  const auto current_pid = static_cast<qlonglong>(QCoreApplication::applicationPid());
  auto state = readJsonMap(statePath());
  const auto state_pid = state.value(QStringLiteral("currentPid"), current_pid).toLongLong();
  // A detached replacement process may already have taken ownership of the
  // recovery state before this process finishes its event loop. Never mark the
  // replacement process as stopped.
  if (state_pid != current_pid) return;

  const auto marker = readJsonMap(markerPath());
  const auto marker_pid = marker.value(QStringLiteral("pid"), qlonglong{-1}).toLongLong();
  if (marker.isEmpty() || marker_pid == current_pid) QFile::remove(markerPath());

  state.insert(QStringLiteral("consecutiveUncleanStarts"), 0);
  state.insert(QStringLiteral("currentSafeMode"), false);
  state.insert(QStringLiteral("currentPid"), qlonglong{-1});
  state.insert(QStringLiteral("automaticSafeMode"), false);
  state.insert(QStringLiteral("currentPhase"), QStringLiteral("stopped"));
  state.insert(QStringLiteral("lastCleanExitAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  (void)writeJsonMap(statePath(), state);
}

QVariantMap RecoveryService::loadState() const {
  auto result = readJsonMap(statePath());
  result.insert(QStringLiteral("recoveryDirectory"), recoveryRoot());
  result.insert(QStringLiteral("safeMode"), result.value(QStringLiteral("currentSafeMode"), false));
  result.insert(QStringLiteral("recoveryPromptPending"), result.value(QStringLiteral("pendingIncident"), false));
  return result;
}

bool RecoveryService::saveState(const QVariantMap& state) const { return writeJsonMap(statePath(), state); }

QVariantMap RecoveryService::state() const { return loadState(); }

bool RecoveryService::safeMode() const {
  return loadState().value(QStringLiteral("currentSafeMode"), false).toBool();
}

bool RecoveryService::recoveryPending() const {
  return loadState().value(QStringLiteral("pendingIncident"), false).toBool();
}

QString RecoveryService::recoveryDirectory() const { return recoveryRoot(); }

ServiceResult RecoveryService::acknowledge() {
  auto value = loadState();
  value.insert(QStringLiteral("pendingIncident"), false);
  if (!saveState(value)) {
    return ServiceResult::failure(QStringLiteral("Recovery state"),
                                  QStringLiteral("Xenon could not update the recovery state file."));
  }
  emit changed();
  return ServiceResult::success();
}

ServiceResult RecoveryService::markInteractive() {
  updateBootstrapPhase(QStringLiteral("interactive"));
  emit changed();
  return ServiceResult::success();
}

ServiceResult RecoveryService::restart(bool safe_mode) {
  const auto executable = QCoreApplication::applicationFilePath();
  if (executable.trimmed().isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Restart Xenon"),
                                  QStringLiteral("The launcher executable path is unavailable."));
  }

  // A deliberate restart must not look like a crash to the next process. If
  // spawning fails, restore a marker for this still-running process.
  const auto current_safe_mode = this->safeMode();
  const auto previous_state = loadState();
  markProcessCleanExit();
  if (!QProcess::startDetached(executable, restartArguments(safe_mode))) {
    (void)saveState(previous_state);
    createCurrentMarker(current_safe_mode, QStringLiteral("restart-failed"));
    emit changed();
    return ServiceResult::failure(QStringLiteral("Restart Xenon"),
                                  QStringLiteral("Xenon could not start a replacement launcher process."));
  }

  QCoreApplication::quit();
  return ServiceResult::success(safe_mode ? QStringLiteral("Restarting in Safe Mode")
                                          : QStringLiteral("Restarting Xenon"));
}

}  // namespace xenon::launcher
