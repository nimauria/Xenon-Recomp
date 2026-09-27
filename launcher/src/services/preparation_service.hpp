#pragma once

// Launcher-side wrapper around the out-of-process automatic game preparation
// worker (xenon-prepare, tools/xenon_prepare.cpp) - see
// docs/development/GAME_PREPARATION.md for the full pipeline and CLI contract. The
// (potentially minutes-long) native compilation always runs in a separate
// process; this class only ever spawns it and polls its status file, so it
// never blocks the launcher's own Qt event loop / UI thread.

#include "service_result.hpp"

#include <QHash>
#include <QObject>
#include <QString>

class QProcess;
class QTimer;

namespace xenon::launcher {

class LibraryService;
class ModuleService;
class PathService;

class PreparationService final : public QObject {
  Q_OBJECT

 public:
  PreparationService(PathService& paths, ModuleService& modules, LibraryService& library,
                     QObject* parent = nullptr);
  ~PreparationService() override;

  // True when this game's assigned module relies on Xenon's automatic
  // preparation pipeline - it declares an analysis-hint package
  // (<modulePath>/xenon-analysis/manifest.json) and does not ship a
  // pre-built native extension of its own. False (nothing to prepare;
  // configurationFor() already resolves the module's shipped native
  // extension) for a traditional module and for a game with no module yet.
  [[nodiscard]] bool usesAutomaticPreparation(const QString& game_id) const;

  // Stable per-library-entry runtime-learning trace consumed by xenon-prepare
  // on the next cache check/build. This lives outside ephemeral runtime
  // session directories so observations survive launcher/game restarts.
  [[nodiscard]] QString adaptiveObservationPath(const QString& game_id) const;

  // Fast, bounded, synchronous check (parses the XEX header and consults the
  // artifact cache only - never compiles) - safe to call from the UI thread.
  // On success with an empty result, `outNativeExtensionPath` is left empty
  // and the caller should call beginPreparation(). Returns failure only for
  // a genuine error (unreadable content, invalid XEX, no module hint data
  // for this exact revision) - never merely "not prepared yet".
  [[nodiscard]] ServiceResult checkCache(const QString& game_id,
                                         QString& outNativeExtensionPath) const;

  // Starts asynchronous preparation for `game_id`. Emits progress()
  // repeatedly and exactly one terminal finished(). A call while one is
  // already running for this game_id is ignored.
  void beginPreparation(const QString& game_id);
  // Cooperative cancellation (creates the worker's --stop-signal file, see
  // Part 19): the worker terminates its own compiler child process tree and
  // exits cleanly on its own; finished(false, ...) still follows.
  void cancel(const QString& game_id);
  [[nodiscard]] bool isPreparing(const QString& game_id) const noexcept;

 signals:
  void progress(const QString& gameId, const QString& phase, int percent, const QString& task);
  void finished(const QString& gameId, bool success, const QString& nativeExtensionPath,
               const QString& errorMessage);

 private:
  struct RunState {
    QProcess* process{nullptr};
    QTimer* pollTimer{nullptr};
    QString statusFile;
    QString stopSignal;
    QString lastPhase;
  };

  [[nodiscard]] QString workerExecutablePath() const;
  [[nodiscard]] QString hintPackagePath(const QString& module_id) const;
  [[nodiscard]] bool buildArguments(const QString& game_id, QStringList& outArgs,
                                    QString& outStatusFile, QString& outStopSignal,
                                    QString& outError) const;
  void pollStatus(const QString& game_id);
  void finishRun(const QString& game_id, bool success, const QString& nativeExtensionPath,
                const QString& errorMessage);

  PathService& paths_;
  ModuleService& modules_;
  LibraryService& library_;
  QHash<QString, RunState> running_;
};

}  // namespace xenon::launcher
