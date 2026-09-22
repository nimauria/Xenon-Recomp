#include "preparation_service.hpp"

#include "library_service.hpp"
#include "module_service.hpp"
#include "path_service.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>

namespace xenon::launcher {
namespace {
#if defined(Q_OS_WIN)
constexpr auto kWorkerExecutable = "xenon-prepare.exe";
#else
constexpr auto kWorkerExecutable = "xenon-prepare";
#endif

QJsonObject readJsonObjectFile(const QString& path) {
  QFile file{path};
  if (!file.open(QIODevice::ReadOnly)) return {};
  const auto document = QJsonDocument::fromJson(file.readAll());
  return document.isObject() ? document.object() : QJsonObject{};
}
}  // namespace

PreparationService::PreparationService(PathService& paths, ModuleService& modules,
                                       LibraryService& library, QObject* parent)
    : QObject(parent), paths_(paths), modules_(modules), library_(library) {}

PreparationService::~PreparationService() {
  for (auto it = running_.begin(); it != running_.end(); ++it) {
    if (it->process) it->process->deleteLater();
    if (it->pollTimer) it->pollTimer->deleteLater();
  }
}

QString PreparationService::workerExecutablePath() const {
  const auto override_path = qEnvironmentVariable("XENON_PREPARE_WORKER_PATH");
  if (!override_path.isEmpty()) return override_path;
  return QDir(QCoreApplication::applicationDirPath()).filePath(QString::fromLatin1(kWorkerExecutable));
}

QString PreparationService::hintPackagePath(const QString& module_id) const {
  const auto module_path = modules_.modulePath(module_id);
  if (module_path.isEmpty()) return {};
  // Fixed convention bridging the launcher's module-package manifest and the
  // Recomp Driver's separate FileModuleHintProvider package layout (see
  // docs/development/GAME_PREPARATION.md "Module hint package convention" and
  // include/xenon/recomp/module_hint_provider.hpp) - both concepts can live
  // in a single installed module directory without either owning the other.
  return QDir(module_path).filePath(QStringLiteral("xenon-analysis"));
}

bool PreparationService::usesAutomaticPreparation(const QString& game_id) const {
  const auto game = library_.entry(game_id);
  const auto module_id = game.value(QStringLiteral("moduleId")).toString().trimmed();
  if (module_id.isEmpty()) return false;
  if (!modules_.nativeExtensionPath(module_id).isEmpty()) return false;  // module ships its own
  const auto hint_manifest = QDir(hintPackagePath(module_id)).filePath(QStringLiteral("manifest.json"));
  return QFileInfo::exists(hint_manifest);
}

bool PreparationService::buildArguments(const QString& game_id, QStringList& outArgs,
                                        QString& outStatusFile, QString& outStopSignal,
                                        QString& outError) const {
  const auto game = library_.entry(game_id);
  if (game.isEmpty()) {
    outError = QStringLiteral("The selected game is not in the launcher library.");
    return false;
  }
  const auto content_path = game.value(QStringLiteral("contentPath")).toString();
  if (content_path.isEmpty() || !QFileInfo::exists(content_path)) {
    outError = QStringLiteral("The game's registered content path no longer exists.");
    return false;
  }
  const auto module_id = game.value(QStringLiteral("moduleId")).toString().trimmed();
  if (module_id.isEmpty()) {
    outError = QStringLiteral("This game does not have a Xenon module assigned yet.");
    return false;
  }

  const auto status_dir = QDir(paths_.preparationCachePath()).filePath(QStringLiteral("status"));
  if (!paths_.ensureDirectory(status_dir)) {
    outError = QStringLiteral("Could not create the preparation status directory.");
    return false;
  }
  outStatusFile = QDir(status_dir).filePath(game_id + QStringLiteral(".status.json"));
  outStopSignal = QDir(status_dir).filePath(game_id + QStringLiteral(".stop"));
  QFile::remove(outStopSignal);  // clear any stale request from a previous run

  outArgs = {QStringLiteral("--content"), content_path,
            QStringLiteral("--module"), hintPackagePath(module_id),
            QStringLiteral("--module-id"), module_id,
            QStringLiteral("--cache-root"), paths_.preparationCachePath(),
            QStringLiteral("--status-file"), outStatusFile,
            QStringLiteral("--stop-signal"), outStopSignal};
  return true;
}

ServiceResult PreparationService::checkCache(const QString& game_id,
                                             QString& outNativeExtensionPath) const {
  outNativeExtensionPath.clear();
  if (!usesAutomaticPreparation(game_id)) {
    return ServiceResult::success(QStringLiteral("No automatic preparation required"));
  }

  QStringList args;
  QString status_file, stop_signal, build_error;
  if (!buildArguments(game_id, args, status_file, stop_signal, build_error)) {
    return ServiceResult::failure(QStringLiteral("Preparation check failed"), build_error);
  }
  args.append(QStringLiteral("--query"));

  const auto worker = workerExecutablePath();
  if (!QFileInfo::exists(worker)) {
    return ServiceResult::failure(
        QStringLiteral("Preparation worker missing"),
        QStringLiteral("Could not find %1 next to the launcher.").arg(QString::fromLatin1(kWorkerExecutable)));
  }

  QProcess process;
  process.start(worker, args);
  if (!process.waitForStarted(5000) || !process.waitForFinished(15000)) {
    return ServiceResult::failure(QStringLiteral("Preparation check failed"),
                                  QStringLiteral("The preparation worker did not respond in time."));
  }
  const auto output = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
  if (!output.value(QStringLiteral("ok")).toBool()) {
    return ServiceResult::failure(
        QStringLiteral("Preparation check failed"),
        output.value(QStringLiteral("error")).toString(QStringLiteral("Unknown preparation worker error.")));
  }
  if (!output.value(QStringLiteral("needsPreparation")).toBool(true)) {
    outNativeExtensionPath = output.value(QStringLiteral("nativeExtensionPath")).toString();
  }
  return ServiceResult::success(QStringLiteral("Preparation status checked"));
}

void PreparationService::beginPreparation(const QString& game_id) {
  if (running_.contains(game_id)) return;

  QStringList args;
  QString status_file, stop_signal, build_error;
  if (!buildArguments(game_id, args, status_file, stop_signal, build_error)) {
    emit finished(game_id, false, {}, build_error);
    return;
  }
  const auto worker = workerExecutablePath();
  if (!QFileInfo::exists(worker)) {
    emit finished(game_id, false, {},
                 QStringLiteral("Could not find %1 next to the launcher.")
                     .arg(QString::fromLatin1(kWorkerExecutable)));
    return;
  }

  auto* process = new QProcess(this);
  auto* timer = new QTimer(this);
  RunState state;
  state.process = process;
  state.pollTimer = timer;
  state.statusFile = status_file;
  state.stopSignal = stop_signal;
  running_.insert(game_id, state);

  timer->setInterval(200);
  connect(timer, &QTimer::timeout, this, [this, game_id]() { pollStatus(game_id); });

  connect(process, &QProcess::finished, this,
          [this, game_id](int exit_code, QProcess::ExitStatus exit_status) {
            pollStatus(game_id);  // pick up the final status write before deciding the outcome
            if (!running_.contains(game_id)) return;
            const auto native_extension = readJsonObjectFile(running_[game_id].statusFile)
                                              .value(QStringLiteral("nativeExtensionPath"))
                                              .toString();
            const bool success = exit_status == QProcess::NormalExit && exit_code == 0 &&
                                 !native_extension.isEmpty();
            QString error_message;
            if (!success) {
              const auto status = readJsonObjectFile(running_[game_id].statusFile);
              error_message = status.value(QStringLiteral("error"))
                                  .toString(QStringLiteral("Preparation failed (exit code %1).").arg(exit_code));
            }
            finishRun(game_id, success, native_extension, error_message);
          });

  process->start(worker, args);
  timer->start();
}

void PreparationService::cancel(const QString& game_id) {
  const auto it = running_.constFind(game_id);
  if (it == running_.constEnd()) return;
  QFile stop_signal{it->stopSignal};
  stop_signal.open(QIODevice::WriteOnly | QIODevice::Truncate);
}

bool PreparationService::isPreparing(const QString& game_id) const noexcept {
  return running_.contains(game_id);
}

void PreparationService::pollStatus(const QString& game_id) {
  const auto it = running_.find(game_id);
  if (it == running_.end()) return;
  const auto status = readJsonObjectFile(it->statusFile);
  if (status.isEmpty()) return;
  const auto phase = status.value(QStringLiteral("phase")).toString();
  const auto percent = status.value(QStringLiteral("percent")).toInt(-1);
  const auto task = status.value(QStringLiteral("task")).toString();
  if (phase == it->lastPhase && task.isEmpty()) return;
  it->lastPhase = phase;
  emit progress(game_id, phase, percent, task);
}

void PreparationService::finishRun(const QString& game_id, bool success,
                                   const QString& nativeExtensionPath,
                                   const QString& errorMessage) {
  const auto it = running_.find(game_id);
  if (it != running_.end()) {
    it->pollTimer->stop();
    it->pollTimer->deleteLater();
    it->process->deleteLater();
    running_.erase(it);
  }
  emit finished(game_id, success, nativeExtensionPath, errorMessage);
}

}  // namespace xenon::launcher
