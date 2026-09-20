#pragma once

#include "../models/launch_configuration.hpp"
#include "../services/service_result.hpp"

#include <QVariantMap>

#include <memory>
#include <optional>

namespace xenon::launcher {

class IRuntimeBridge {
 public:
  virtual ~IRuntimeBridge() = default;

  [[nodiscard]] virtual ServiceResult connect() = 0;
  virtual void disconnect() = 0;
  [[nodiscard]] virtual bool connected() const noexcept = 0;
  [[nodiscard]] virtual QString status() const = 0;
  [[nodiscard]] virtual QVariantMap capabilities() const = 0;

  [[nodiscard]] virtual ServiceResult prepareLaunch(const LaunchConfiguration& configuration) const = 0;
  [[nodiscard]] virtual ServiceResult launch(const LaunchConfiguration& configuration) = 0;
  [[nodiscard]] virtual ServiceResult stop() = 0;

  // Extended status APIs
  [[nodiscard]] virtual QVariantMap sessionStatus() const = 0;
  [[nodiscard]] virtual QString runtimeLog() const = 0;
};

// Supervises the generic Xenon runtime/game-host process (xenon_runtime_host)
// instead of executing guest code in the launcher itself. See
// docs/RUNTIME_HOST.md for the process/IPC contract this implements: a JSON
// launch configuration handed to a detached child process, and
// status.json/log.txt/stop.signal files in a per-session directory used to
// supervise it afterward. Running detached is what lets a game outlive the
// launcher process (intentional/crash exit alike).
class RuntimeBridge final : public IRuntimeBridge {
 public:
  RuntimeBridge();
  ~RuntimeBridge() override;

  RuntimeBridge(const RuntimeBridge&) = delete;
  RuntimeBridge& operator=(const RuntimeBridge&) = delete;

  [[nodiscard]] ServiceResult connect() override;
  void disconnect() override;
  [[nodiscard]] bool connected() const noexcept override;
  [[nodiscard]] QString status() const override;
  [[nodiscard]] QVariantMap capabilities() const override;

  [[nodiscard]] ServiceResult prepareLaunch(const LaunchConfiguration& configuration) const override;
  [[nodiscard]] ServiceResult launch(const LaunchConfiguration& configuration) override;
  [[nodiscard]] ServiceResult stop() override;

  // Extended status APIs
  [[nodiscard]] QVariantMap sessionStatus() const override;
  [[nodiscard]] QString runtimeLog() const override;

 private:
  [[nodiscard]] QString runtimeHostPath() const;
  [[nodiscard]] QString sessionsRootDirectory() const;
  [[nodiscard]] QString currentSessionDirectory() const;
  [[nodiscard]] QVariantMap readStatusFile() const;
  // readStatusFile() plus liveness-based crash detection: if the runtime
  // host's OS process has exited without status.json ever reaching a
  // terminal state, synthesizes a "crashed" status instead of reporting
  // stale/missing data as if the session were still progressing. See
  // docs/RUNTIME_HOST.md "Detecting a crash".
  [[nodiscard]] QVariantMap augmentedStatus() const;
  // Best-effort liveness/exit-code probe for a process this launcher does
  // not own as a child (the runtime host always runs detached - see
  // docs/RUNTIME_HOST.md). `determinable` is false when the platform could
  // not give a trustworthy answer (e.g. permission denied), in which case
  // callers must not treat that as either "alive" or "dead".
  struct ProcessState {
    bool determinable = false;
    bool alive = false;
    long exit_code = -1;
  };
  // On Windows, prefers `handle` (a HANDLE opened right after this session's
  // process was spawned and held by this bridge - see launch()/closeSessionProcessHandle())
  // over opening a fresh one by pid. A detached process has nothing else
  // holding a handle to it, so a handle opened only at query time can lose
  // the race against the OS recycling the pid once the process has already
  // exited, silently losing the exit code. `handle` is unused on POSIX,
  // where liveness is checked by pid regardless (see queryProcessState()'s
  // definition).
  [[nodiscard]] static ProcessState queryProcessState(qint64 pid, void* handle);
  // Closes and clears current_session_process_handle_ if open. Safe to call
  // when already closed.
  void closeSessionProcessHandle() noexcept;
  void logRuntime(const QString& message) const;

  bool runtime_host_located_ = false;
  QString runtime_host_path_;
  QString current_session_id_;
  qint64 current_session_pid_ = -1;
  // Opaque HANDLE (Windows) opened immediately after spawning the current
  // session's process; see queryProcessState(). Always null on POSIX.
  void* current_session_process_handle_ = nullptr;
  mutable bool crash_logged_ = false;
  mutable QString runtime_log_;
};

}  // namespace xenon::launcher
