#include "runtime_bridge.hpp"

#include "xenon/core/json.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>

#include <chrono>
#include <thread>

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#endif

#ifndef XENON_LAUNCHER_RUNTIME_MEMORY
#define XENON_LAUNCHER_RUNTIME_MEMORY 0
#endif
#ifndef XENON_LAUNCHER_RUNTIME_GRAPHICS
#define XENON_LAUNCHER_RUNTIME_GRAPHICS 0
#endif
#ifndef XENON_LAUNCHER_RUNTIME_AUDIO
#define XENON_LAUNCHER_RUNTIME_AUDIO 0
#endif
#ifndef XENON_LAUNCHER_RUNTIME_INPUT
#define XENON_LAUNCHER_RUNTIME_INPUT 0
#endif
#ifndef XENON_LAUNCHER_RUNTIME_NETWORK
#define XENON_LAUNCHER_RUNTIME_NETWORK 0
#endif
#ifndef XENON_LAUNCHER_RUNTIME_VULKAN
#define XENON_LAUNCHER_RUNTIME_VULKAN 0
#endif
#ifndef XENON_LAUNCHER_RUNTIME_D3D12
#define XENON_LAUNCHER_RUNTIME_D3D12 0
#endif

namespace xenon::launcher {

namespace {

#if defined(Q_OS_WIN)
constexpr auto kRuntimeHostExecutable = "xenon_runtime_host.exe";
#else
constexpr auto kRuntimeHostExecutable = "xenon_runtime_host";
#endif

// Converts a parsed JSON value (see xenon/core/json.hpp) into the QVariant
// tree the rest of the launcher already works with, so RuntimeBridge can
// hand session status straight to frontend_backend/QML without a second
// representation.
QVariant jsonToVariant(const xenon::core::JsonValue& value) {
  switch (value.type()) {
    case xenon::core::JsonValue::Type::Null:
      return {};
    case xenon::core::JsonValue::Type::Bool:
      return value.as_bool();
    case xenon::core::JsonValue::Type::Number:
      return value.as_number();
    case xenon::core::JsonValue::Type::String:
      return QString::fromStdString(value.as_string());
    case xenon::core::JsonValue::Type::Array: {
      QVariantList list;
      if (const auto* array = value.as_array()) {
        for (const auto& entry : *array) list.append(jsonToVariant(entry));
      }
      return list;
    }
    case xenon::core::JsonValue::Type::Object: {
      QVariantMap map;
      if (const auto* object = value.as_object()) {
        for (const auto& [key, entry] : *object) {
          map.insert(QString::fromStdString(key), jsonToVariant(entry));
        }
      }
      return map;
    }
  }
  return {};
}

// Helper: Find default.xex in content path (used only for a pre-flight
// existence check; the runtime host resolves it again for itself since it
// is the process that actually reads the file).
bool contentHasDefaultXex(const QString& content_path) {
  for (const auto* name : {"default.xex", "Default.xex", "DEFAULT.XEX"}) {
    if (QFileInfo::exists(QDir(content_path).filePath(QString::fromLatin1(name)))) return true;
  }
  return false;
}

QString findTitleUpdatePath(const LaunchConfiguration& config) {
  for (const auto& dlc_entry : config.dlc) {
    const auto dlc_map = dlc_entry.toMap();
    const auto type = dlc_map.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("TitleUpdate") || type == QStringLiteral("Update")) {
      const auto path = dlc_map.value(QStringLiteral("path")).toString();
      if (!path.isEmpty()) return path;
    }
  }
  return {};
}

QString findDlcRootPath(const LaunchConfiguration& config) {
  if (!config.dlc.isEmpty()) {
    const auto first_dlc = config.dlc.first().toMap();
    const auto path = first_dlc.value(QStringLiteral("path")).toString();
    if (!path.isEmpty()) return QFileInfo(path).absolutePath();
  }
  if (!config.managed_game_path.isEmpty()) {
    return QDir(config.managed_game_path).filePath(QStringLiteral("DLC"));
  }
  return {};
}

}  // namespace

RuntimeBridge::RuntimeBridge() = default;
RuntimeBridge::~RuntimeBridge() { closeSessionProcessHandle(); }

void RuntimeBridge::closeSessionProcessHandle() noexcept {
#if defined(Q_OS_WIN)
  if (current_session_process_handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(current_session_process_handle_));
  }
#endif
  current_session_process_handle_ = nullptr;
}

QString RuntimeBridge::runtimeHostPath() const { return runtime_host_path_; }

ServiceResult RuntimeBridge::connect() {
  // "Connecting" here means locating the generic runtime/game-host binary
  // this launcher build ships alongside - there is nothing to keep an
  // in-process connection to any more (see docs/RUNTIME_HOST.md). Every Play
  // action spawns its own detached xenon_runtime_host process.
  //
  // XENON_RUNTIME_HOST_PATH lets tests (and advanced dev workflows) point at
  // a specific runtime host binary instead of relying on it sitting next to
  // this launcher executable. It is never required for a normal install.
  const auto override_path = qEnvironmentVariable("XENON_RUNTIME_HOST_PATH");
  const auto candidate = !override_path.isEmpty()
      ? override_path
      : QDir(QCoreApplication::applicationDirPath()).filePath(QString::fromLatin1(kRuntimeHostExecutable));
  if (!QFileInfo::exists(candidate)) {
    runtime_host_located_ = false;
    return ServiceResult::failure(
        QStringLiteral("Runtime host missing"),
        QStringLiteral("Could not find %1 next to the launcher. Reinstall Xenon or rebuild it "
                       "with the runtime host target enabled.")
            .arg(QString::fromLatin1(kRuntimeHostExecutable)));
  }
  runtime_host_path_ = candidate;
  runtime_host_located_ = true;
  return ServiceResult::success(
      QStringLiteral("Runtime host found"),
      QStringLiteral("The launcher can spawn the Xenon runtime host to run games."));
}

void RuntimeBridge::disconnect() {
  // Intentionally does not touch any running session: a game the launcher
  // spawned keeps running after this launcher instance stops caring about
  // it, by design (runtime separation).
  runtime_host_located_ = false;
}

bool RuntimeBridge::connected() const noexcept { return runtime_host_located_; }

QString RuntimeBridge::status() const {
  return connected() ? QStringLiteral("Connected") : QStringLiteral("Disconnected");
}

QVariantMap RuntimeBridge::capabilities() const {
  QVariantMap result;
  result.insert(QStringLiteral("core"), true);
  result.insert(QStringLiteral("connected"), connected());
  // These report what this launcher build expects its paired
  // xenon_runtime_host build to support (they are compiled into the
  // launcher, mirroring the runtime's own build options). Input additionally
  // has a live launcher feature and a versioned module API.
  result.insert(QStringLiteral("memoryCompiled"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_MEMORY));
  result.insert(QStringLiteral("graphicsCompiled"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_GRAPHICS));
  result.insert(QStringLiteral("audioCompiled"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_AUDIO));
  result.insert(QStringLiteral("networkCompiled"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_NETWORK));
  result.insert(QStringLiteral("vulkanCompiled"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_VULKAN));
  result.insert(QStringLiteral("d3d12Compiled"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_D3D12));
  result.insert(QStringLiteral("memory"), false);
  result.insert(QStringLiteral("graphics"), false);
  result.insert(QStringLiteral("audio"), false);
  result.insert(QStringLiteral("inputCompiled"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_INPUT));
  result.insert(QStringLiteral("input"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_INPUT));
  result.insert(QStringLiteral("inputModuleApiVersion"), XENON_LAUNCHER_RUNTIME_INPUT ? 1 : 0);
  result.insert(QStringLiteral("network"), false);
  result.insert(QStringLiteral("sessionLaunch"), true);
  return result;
}

ServiceResult RuntimeBridge::prepareLaunch(const LaunchConfiguration& configuration) const {
  if (!connected()) {
    return ServiceResult::failure(QStringLiteral("Runtime unavailable"),
                                  QStringLiteral("The Xenon runtime host was not found."));
  }
  const auto& content = configuration.content_path;
  if (content.trimmed().isEmpty() || !QFileInfo::exists(content)) {
    return ServiceResult::failure(QStringLiteral("Game content missing"),
                                  QStringLiteral("The launch configuration does not point to existing local game content."));
  }
  if (!contentHasDefaultXex(content)) {
    return ServiceResult::failure(
        QStringLiteral("Game executable missing"),
        QStringLiteral("Could not find a default.xex under the game's content path."));
  }
  if (configuration.module_id.trimmed().isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Game module required"),
                                  QStringLiteral("A compatible Xenon game module must identify the content before it can launch."));
  }

  const auto input_requirement =
      configuration.runtime_api_requirements.value(QStringLiteral("input")).toMap();
  if (!input_requirement.isEmpty() &&
      input_requirement.value(QStringLiteral("required"), true).toBool()) {
    const auto requested = input_requirement.value(QStringLiteral("version"), 1).toInt();
    const auto caps = capabilities();
    const auto provided = caps.value(QStringLiteral("inputModuleApiVersion"), 0).toInt();
    if (!caps.value(QStringLiteral("inputCompiled"), false).toBool() || provided <= 0) {
      return ServiceResult::failure(
          QStringLiteral("Input API unavailable"),
          QStringLiteral("This game module requires Xenon Input API v%1, but Input is not compiled into this runtime.").arg(requested));
    }
    if (requested <= 0 || requested > provided) {
      return ServiceResult::failure(
          QStringLiteral("Input API incompatible"),
          QStringLiteral("This game module requires Xenon Input API v%1, but this runtime provides v%2.")
              .arg(requested).arg(provided));
    }
  }
  return ServiceResult::success(QStringLiteral("Launch configuration ready"),
                                QStringLiteral("The launcher configuration passed the runtime-bridge validation boundary."),
                                configuration.toVariantMap());
}

QString RuntimeBridge::sessionsRootDirectory() const {
  const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  return QDir(base).filePath(QStringLiteral("runtime-sessions"));
}

QString RuntimeBridge::currentSessionDirectory() const {
  if (current_session_id_.isEmpty()) return {};
  return QDir(sessionsRootDirectory()).filePath(current_session_id_);
}

ServiceResult RuntimeBridge::launch(const LaunchConfiguration& configuration) {
  const auto prepared = prepareLaunch(configuration);
  if (!prepared.ok) return prepared;

  const auto session_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto session_dir = QDir(sessionsRootDirectory()).filePath(session_id);
  if (!QDir().mkpath(session_dir)) {
    return ServiceResult::failure(QStringLiteral("Session directory failed"),
                                  QStringLiteral("Could not create a runtime session directory."));
  }

  core::JsonValue root = core::JsonValue::make_object();
  // See docs/RUNTIME_HOST.md "Launch configuration version" - bump this only
  // when a field's meaning changes incompatibly; the runtime host rejects a
  // configVersion newer than it supports instead of guessing.
  root.set("configVersion", static_cast<double>(1));
  root.set("sessionId", session_id.toStdString());
  root.set("sessionDir", session_dir.toStdString());
  root.set("gameId", configuration.game_id.toStdString());
  root.set("title", configuration.title.toStdString());
  root.set("contentPath", configuration.content_path.toStdString());
  root.set("moduleId", configuration.module_id.toStdString());
  root.set("moduleName", configuration.module_name.toStdString());
  root.set("modulePath", configuration.module_path.toStdString());
  root.set("moduleVersion", configuration.module_version.toStdString());
  root.set("nativeExtensionPath", configuration.native_extension_path.toStdString());
  root.set("profileId", configuration.profile_id.toStdString());
  root.set("profileName", configuration.profile_name.toStdString());
  root.set("region", configuration.region.toStdString());
  // Placeholder XUID until the profile system exposes a real signed-in XUID
  // (matches the value the previous in-process path used). Encoded as a
  // decimal string, not a JSON number: xenon::core::JsonValue represents
  // numbers as double, whose 53-bit mantissa cannot hold a full 64-bit XUID
  // exactly - a real Xbox User ID round-tripped as a JSON number would
  // silently come out wrong on the runtime host side.
  root.set("profileXuid", QString::number(0xE000000000000001ULL).toStdString());
  root.set("renderer", configuration.renderer.toStdString());
  root.set("inputBackend", configuration.input_backend.toStdString());
  root.set("audioMasterVolume", configuration.audio_master_volume);
  root.set("audioMuteUnfocused", configuration.audio_mute_unfocused);
  root.set("audioLatencyProfile", configuration.audio_latency_profile.toStdString());
  // See docs/RUNTIME_HOST.md: not yet sourced from the launcher's
  // "developer/verboseLogging" setting (RuntimeBridge has no SettingsService
  // access), so this is currently always false.
  root.set("logVerbose", false);
  root.set("titleUpdatePath", findTitleUpdatePath(configuration).toStdString());
  root.set("dlcRootPath", findDlcRootPath(configuration).toStdString());

  core::JsonValue dlc_array = core::JsonValue::make_array();
  for (const auto& dlc_entry : configuration.dlc) {
    const auto dlc_map = dlc_entry.toMap();
    core::JsonValue entry = core::JsonValue::make_object();
    entry.set("type", dlc_map.value(QStringLiteral("type")).toString().toStdString());
    entry.set("path", dlc_map.value(QStringLiteral("path")).toString().toStdString());
    dlc_array.append(std::move(entry));
  }
  root.set("dlc", std::move(dlc_array));

  root.set("savePath", configuration.save_path.toStdString());
  root.set("screenshotsPath", configuration.screenshots_path.toStdString());
  root.set("offline", configuration.offline);

  const auto config_path = QDir(session_dir).filePath(QStringLiteral("launch-config.json"));
  QFile config_file(config_path);
  if (!config_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return ServiceResult::failure(QStringLiteral("Launch configuration failed"),
                                  QStringLiteral("Could not write the runtime host launch configuration."));
  }
  const auto json_text = QString::fromStdString(root.dump());
  config_file.write(json_text.toUtf8());
  config_file.close();

  qint64 pid = -1;
  const auto started = QProcess::startDetached(
      runtime_host_path_, {QStringLiteral("--launch-config"), config_path}, session_dir, &pid);
  if (!started) {
    return ServiceResult::failure(
        QStringLiteral("Runtime host failed to start"),
        QStringLiteral("Could not launch %1.").arg(QString::fromLatin1(kRuntimeHostExecutable)));
  }

  closeSessionProcessHandle();
  current_session_id_ = session_id;
  current_session_pid_ = pid;
#if defined(Q_OS_WIN)
  // Opened immediately after spawning, and held for the life of this
  // session: see queryProcessState()'s doc comment for why a handle opened
  // only when a poll needs it can lose the exit code to a pid-recycling
  // race against a detached process that has already exited.
  current_session_process_handle_ =
      ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
#endif
  crash_logged_ = false;
  logRuntime(QStringLiteral("Launched %1 (session %2, pid %3)")
                 .arg(configuration.title, session_id, QString::number(pid)));

  return ServiceResult::success(QStringLiteral("Game launched"),
                                QStringLiteral("The Xenon runtime host is starting the game session."),
                                configuration.toVariantMap());
}

ServiceResult RuntimeBridge::stop() {
  if (current_session_id_.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Session unavailable"),
                                  QStringLiteral("There is no active runtime session to stop."));
  }

  const auto stop_signal_path = QDir(currentSessionDirectory()).filePath(QStringLiteral("stop.signal"));
  QFile stop_signal(stop_signal_path);
  if (!stop_signal.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return ServiceResult::failure(QStringLiteral("Stop failed"),
                                  QStringLiteral("Could not signal the runtime host to stop."));
  }
  stop_signal.close();
  logRuntime(QStringLiteral("Stop requested for session %1").arg(current_session_id_));

  // Give a clean/fast stop a brief chance to land before returning, without
  // noticeably blocking the UI thread. A game that does not exit
  // cooperatively is still bounded by the runtime host's own hard-stop
  // timeout (see docs/RUNTIME_HOST.md) - this call does not wait for that.
  constexpr int kPollIntervalMs = 50;
  constexpr int kMaxWaitMs = 500;
  for (int waited = 0; waited < kMaxWaitMs; waited += kPollIntervalMs) {
    const auto status = augmentedStatus();
    const auto state_name = status.value(QStringLiteral("stateName")).toString();
    if (state_name == QStringLiteral("stopped") || state_name == QStringLiteral("failed") ||
        state_name == QStringLiteral("crashed") ||
        !status.value(QStringLiteral("available"), false).toBool()) {
      current_session_id_.clear();
      current_session_pid_ = -1;
      closeSessionProcessHandle();
      return ServiceResult::success(QStringLiteral("Session stopped"),
                                    QStringLiteral("The game session has ended."));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
  }

  return ServiceResult::success(
      QStringLiteral("Stop requested"),
      QStringLiteral("Xenon asked the runtime host to stop; the game will end shortly."));
}

QVariantMap RuntimeBridge::readStatusFile() const {
  QVariantMap result;
  if (current_session_id_.isEmpty()) {
    result.insert(QStringLiteral("available"), false);
    return result;
  }
  const auto status_path = QDir(currentSessionDirectory()).filePath(QStringLiteral("status.json"));
  QFile file(status_path);
  if (!file.open(QIODevice::ReadOnly)) {
    result.insert(QStringLiteral("available"), false);
    return result;
  }
  const auto bytes = file.readAll();
  file.close();

  core::JsonValue root;
  std::string parse_error;
  if (!core::JsonValue::parse(bytes.toStdString(), root, &parse_error)) {
    result.insert(QStringLiteral("available"), false);
    return result;
  }
  auto variant = jsonToVariant(root);
  return variant.toMap();
}

RuntimeBridge::ProcessState RuntimeBridge::queryProcessState(qint64 pid, void* handle) {
  ProcessState result;
  if (pid <= 0) {
    result.determinable = true;
    result.alive = false;
    return result;
  }
  static_cast<void>(handle);  // Windows-only; POSIX liveness is checked by pid regardless.
#if defined(Q_OS_WIN)
  // Prefer the handle opened at spawn time (see launch()): a detached
  // process has nothing else holding a reference to it, so the OS is free to
  // recycle its pid as soon as it exits. Opening a fresh handle by pid here
  // instead can lose that race and silently return "determinable-dead, no
  // exit code" for a pid that has already been reused, or fail outright.
  HANDLE process = static_cast<HANDLE>(handle);
  const bool owns_process_handle = process == nullptr;
  if (owns_process_handle) {
    process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  }
  if (process == nullptr) {
    // A stale/reused pid reports ERROR_INVALID_PARAMETER here, which is the
    // common case for "the process already exited". Treat it as
    // determinable-dead rather than as a permission failure, since this
    // launcher only ever queries pids it spawned itself.
    result.determinable = true;
    result.alive = false;
    return result;
  }
  DWORD exit_code = 0;
  if (::GetExitCodeProcess(process, &exit_code)) {
    result.determinable = true;
    result.alive = (exit_code == STILL_ACTIVE);
    if (!result.alive) result.exit_code = static_cast<long>(exit_code);
  }
  if (owns_process_handle) ::CloseHandle(process);
#else
  if (::kill(static_cast<pid_t>(pid), 0) == 0) {
    result.determinable = true;
    result.alive = true;
  } else if (errno == ESRCH) {
    result.determinable = true;
    result.alive = false;
    // A detached, non-child process's exit code cannot be retrieved on POSIX
    // without being the parent that reaps it (no waitpid() is possible here
    // - see docs/RUNTIME_HOST.md). Liveness is still reliable.
  }
  // Any other errno (e.g. EPERM) leaves `determinable` false: avoid reporting
  // a crash we cannot actually confirm.
#endif
  return result;
}

QVariantMap RuntimeBridge::augmentedStatus() const {
  auto status = readStatusFile();
  if (current_session_id_.isEmpty()) return status;

  const auto state_name = status.value(QStringLiteral("stateName")).toString();
  const bool terminal = state_name == QStringLiteral("stopped") ||
                        state_name == QStringLiteral("failed") ||
                        state_name == QStringLiteral("crashed");
  if (terminal) return status;

  const auto process_state = queryProcessState(current_session_pid_, current_session_process_handle_);
  if (!process_state.determinable || process_state.alive) return status;

  // The OS process is confirmed gone but status.json never reached a
  // terminal state - the runtime host crashed (or was killed) instead of
  // shutting down cleanly. Synthesize a bridge-only "crashed" state rather
  // than let the launcher keep showing stale "running"/"initializing" data
  // for a process that no longer exists. See docs/RUNTIME_HOST.md.
  if (!crash_logged_) {
    logRuntime(QStringLiteral("Runtime host process (pid %1) exited unexpectedly without reaching a terminal state")
                   .arg(current_session_pid_));
    crash_logged_ = true;
  }
  status.insert(QStringLiteral("available"), true);
  status.insert(QStringLiteral("sessionId"), current_session_id_);
  status.insert(QStringLiteral("state"), -1);
  status.insert(QStringLiteral("stateName"), QStringLiteral("crashed"));
  status.insert(QStringLiteral("initialized"), false);
  status.insert(QStringLiteral("running"), false);
  status.insert(QStringLiteral("executionActive"), false);
  if (status.value(QStringLiteral("lastError")).toString().isEmpty()) {
    status.insert(QStringLiteral("lastError"),
                  QStringLiteral("The Xenon runtime host exited unexpectedly before reaching a clean state."));
  }
  if (process_state.exit_code >= 0) {
    status.insert(QStringLiteral("exitCode"), static_cast<qlonglong>(process_state.exit_code));
  }
  return status;
}

QVariantMap RuntimeBridge::sessionStatus() const { return augmentedStatus(); }

QString RuntimeBridge::runtimeLog() const {
  if (!current_session_id_.isEmpty()) {
    const auto log_path = QDir(currentSessionDirectory()).filePath(QStringLiteral("log.txt"));
    QFile file(log_path);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      // Tail the last 10000 characters, matching the previous in-process
      // buffer's retention policy.
      constexpr qint64 kMaxTail = 10000;
      if (file.size() > kMaxTail) file.seek(file.size() - kMaxTail);
      const auto contents = QString::fromUtf8(file.readAll());
      return runtime_log_.isEmpty() ? contents : runtime_log_ + contents;
    }
  }
  return runtime_log_;
}

void RuntimeBridge::logRuntime(const QString& message) const {
  const auto timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
  const auto line = QString("[%1] %2\n").arg(timestamp, message);
  runtime_log_.append(line);
  if (runtime_log_.size() > 10000) {
    runtime_log_ = runtime_log_.right(10000);
  }
}

}  // namespace xenon::launcher
