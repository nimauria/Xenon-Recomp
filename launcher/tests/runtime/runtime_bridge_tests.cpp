// Tests for RuntimeBridge, the launcher-side half of the runtime-host IPC
// contract (docs/runtime/RUNTIME_HOST.md). These exercise RuntimeBridge exactly as
// the launcher UI does - through its public API - against a lightweight test
// double (launcher/tests/fixtures/runtime_host_fixture.cpp) instead of the
// real xenon_runtime_host, so a game module, a native extension, or CPU V2
// compiled code is never required. RuntimeBridge::connect() is told where to
// find that double via XENON_RUNTIME_HOST_PATH, an override it honors
// specifically so this test (and any future one) can run without depending
// on build output layout - see RuntimeBridge::connect().
//
// Coverage maps directly to the "Standalone Runtime Host V1" task's TESTS
// list: runtime starts, config validation (missing module/content), IPC
// lifecycle, launcher closing while the runtime continues, runtime crash,
// clean shutdown, exit codes, and multiple sequential launches.

#include "runtime/runtime_bridge.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

using xenon::launcher::LaunchConfiguration;
using xenon::launcher::RuntimeBridge;

#if defined(Q_OS_WIN)
constexpr auto kFixtureExecutable = "xenon_runtime_host_fixture.exe";
#else
constexpr auto kFixtureExecutable = "xenon_runtime_host_fixture";
#endif

// Waits until sessionStatus()'s stateName is one of `targets`, or `timeoutMs`
// elapses. Returns the last observed status either way; callers assert on
// its contents so a timeout produces a readable failure instead of a hang.
QVariantMap pollUntilState(RuntimeBridge& bridge, const QStringList& targets, int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  QVariantMap status;
  do {
    status = bridge.sessionStatus();
    const auto state_name = status.value(QStringLiteral("stateName")).toString();
    if (targets.contains(state_name)) return status;
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  } while (std::chrono::steady_clock::now() < deadline);
  return status;
}

// A game content directory good enough to pass RuntimeBridge::prepareLaunch()
// (it only checks that the path exists and contains a default.xex - the
// fixture never reads game content itself).
QTemporaryDir makeContentDir() {
  QTemporaryDir dir;
  assert(dir.isValid());
  QFile default_xex(QDir(dir.path()).filePath(QStringLiteral("default.xex")));
  assert(default_xex.open(QIODevice::WriteOnly));
  default_xex.write("not a real xex - just needs to exist");
  default_xex.close();
  return dir;
}

LaunchConfiguration makeConfig(const QString& content_path, const QString& game_id) {
  LaunchConfiguration config{};
  config.game_id = game_id;
  config.title = QStringLiteral("Runtime Bridge Test Title");
  config.content_path = content_path;
  config.module_id = QStringLiteral("org.xenon.test-module");
  config.module_name = QStringLiteral("Test Module");
  config.renderer = QStringLiteral("Automatic");
  config.input_backend = QStringLiteral("Automatic");
  return config;
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("XenonTests"));
  QCoreApplication::setApplicationName(QStringLiteral("runtime_bridge_tests"));
  // Keeps RuntimeBridge's runtime-sessions directory under a disposable,
  // test-mode-specific location instead of the real per-user AppData path.
  QStandardPaths::setTestModeEnabled(true);

  const auto fixture_path =
      QDir(QCoreApplication::applicationDirPath()).filePath(QString::fromLatin1(kFixtureExecutable));
  assert(QFileInfo::exists(fixture_path) &&
         "runtime_host_fixture must be built next to runtime_bridge_tests");
  qputenv("XENON_RUNTIME_HOST_PATH", fixture_path.toUtf8());

  std::cout << "Testing Xenon RuntimeBridge...\n";

  // Test 1: connect() locates the (fixture) runtime host via the override.
  {
    RuntimeBridge bridge;
    const auto result = bridge.connect();
    assert(result.ok && "connect() should succeed when XENON_RUNTIME_HOST_PATH is valid");
    assert(bridge.connected());
  }
  std::cout << "  [PASS] connect() locates the runtime host via XENON_RUNTIME_HOST_PATH\n";

  // Test 1b: connect() fails cleanly when the override points nowhere.
  {
    qputenv("XENON_RUNTIME_HOST_PATH", "Z:/definitely/not/a/real/path.exe");
    RuntimeBridge bridge;
    const auto result = bridge.connect();
    assert(!result.ok && "connect() must fail when the runtime host cannot be found");
    assert(!bridge.connected());
    qputenv("XENON_RUNTIME_HOST_PATH", fixture_path.toUtf8());
  }
  std::cout << "  [PASS] connect() fails when the runtime host binary is missing\n";

  // Test 2: config validation - missing content path is rejected before any
  // process is spawned.
  {
    RuntimeBridge bridge;
    assert(bridge.connect().ok);
    auto config = makeConfig(QStringLiteral("Z:/no/such/content"), QStringLiteral("scenario-normal"));
    const auto result = bridge.prepareLaunch(config);
    assert(!result.ok && "prepareLaunch() must reject a missing content path");
  }
  std::cout << "  [PASS] Config validation rejects missing game content\n";

  // Test 2b: config validation - missing module ID is rejected.
  {
    auto content_dir = makeContentDir();
    RuntimeBridge bridge;
    assert(bridge.connect().ok);
    auto config = makeConfig(content_dir.path(), QStringLiteral("scenario-normal"));
    config.module_id.clear();
    const auto result = bridge.prepareLaunch(config);
    assert(!result.ok && "prepareLaunch() must reject a config with no module_id");
  }
  std::cout << "  [PASS] Config validation rejects a missing game module\n";

  // Test 2c: config validation - content without a default.xex is rejected
  // ("bad module"/bad content shape, caught before ever spawning a process).
  {
    QTemporaryDir empty_dir;
    assert(empty_dir.isValid());
    RuntimeBridge bridge;
    assert(bridge.connect().ok);
    auto config = makeConfig(empty_dir.path(), QStringLiteral("scenario-normal"));
    const auto result = bridge.prepareLaunch(config);
    assert(!result.ok && "prepareLaunch() must reject content with no default.xex");
  }
  std::cout << "  [PASS] Config validation rejects content missing default.xex\n";

  // Test 2d: a fully valid configuration passes prepareLaunch().
  {
    auto content_dir = makeContentDir();
    RuntimeBridge bridge;
    assert(bridge.connect().ok);
    auto config = makeConfig(content_dir.path(), QStringLiteral("scenario-normal"));
    const auto result = bridge.prepareLaunch(config);
    assert(result.ok && "A well-formed configuration should pass prepareLaunch()");
  }
  std::cout << "  [PASS] A well-formed configuration passes prepareLaunch()\n";

  // Test 3: runtime starts, and the IPC lifecycle reaches "running" through
  // its documented intermediate states.
  {
    auto content_dir = makeContentDir();
    RuntimeBridge bridge;
    assert(bridge.connect().ok);
    auto config = makeConfig(content_dir.path(), QStringLiteral("scenario-normal"));
    const auto launch_result = bridge.launch(config);
    assert(launch_result.ok && "launch() should succeed for a valid configuration");

    const auto status = pollUntilState(bridge, {QStringLiteral("running")}, 3000);
    assert(status.value(QStringLiteral("stateName")).toString() == QStringLiteral("running"));
    assert(status.value(QStringLiteral("available")).toBool());
    assert(status.value(QStringLiteral("running")).toBool());
    assert(status.value(QStringLiteral("pid")).toLongLong() > 0);

    // Test 4 (same session): clean shutdown. scenario-normal honors
    // stop.signal well within the bridge's 500ms stop() wait budget, so
    // stop() should observe the terminal state itself.
    const auto stop_result = bridge.stop();
    assert(stop_result.ok);
    assert(stop_result.title == QStringLiteral("Session stopped") &&
           "A fast-stopping session should be observed as stopped before stop() returns");
    // A confirmed stop() clears the bridge's tracked session entirely (see
    // RuntimeBridge::stop()), so a follow-up query reports no active session
    // rather than stale "stopped" data.
    const auto post_stop_status = bridge.sessionStatus();
    assert(!post_stop_status.value(QStringLiteral("available")).toBool());
  }
  std::cout << "  [PASS] Runtime starts, IPC lifecycle reaches running, then clean shutdown\n";

  // Test 5: launcher closing while the runtime continues. disconnect() must
  // not touch the spawned process or this bridge's ability to keep
  // supervising/stopping it - that independence is the whole point of the
  // runtime-host split (see docs/runtime/RUNTIME_HOST.md "Process independence").
  {
    auto content_dir = makeContentDir();
    RuntimeBridge bridge;
    assert(bridge.connect().ok);
    auto config = makeConfig(content_dir.path(), QStringLiteral("scenario-normal"));
    assert(bridge.launch(config).ok);
    pollUntilState(bridge, {QStringLiteral("running")}, 3000);

    bridge.disconnect();
    assert(!bridge.connected() && "disconnect() should report the bridge as disconnected");

    // The session must still be observable and controllable after
    // disconnect(): "closing the launcher" (disconnect) is independent of
    // the game process's lifetime.
    const auto status = bridge.sessionStatus();
    assert(status.value(QStringLiteral("stateName")).toString() == QStringLiteral("running") &&
           "A disconnected bridge must still report the still-running session, not lose it");

    const auto stop_result = bridge.stop();
    assert(stop_result.ok && "stop() must still work after disconnect()");
  }
  std::cout << "  [PASS] Launcher disconnect() does not affect the running session\n";

  // Test 6: runtime crash detection. The fixture writes one "running" status
  // then exits abruptly without ever reaching a terminal state - RuntimeBridge
  // must synthesize "crashed" from process-liveness rather than report stale
  // "running" data forever.
  {
    auto content_dir = makeContentDir();
    RuntimeBridge bridge;
    assert(bridge.connect().ok);
    auto config = makeConfig(content_dir.path(), QStringLiteral("scenario-crash"));
    assert(bridge.launch(config).ok);

    const auto status = pollUntilState(bridge, {QStringLiteral("crashed")}, 3000);
    assert(status.value(QStringLiteral("stateName")).toString() == QStringLiteral("crashed") &&
           "RuntimeBridge should detect the runtime host process exiting without a terminal status");
    assert(!status.value(QStringLiteral("running")).toBool());
    assert(!status.value(QStringLiteral("lastError")).toString().isEmpty());

#if defined(Q_OS_WIN)
    // Exit code recovery is Windows-only (see docs/runtime/RUNTIME_HOST.md "Detecting
    // a crash" - POSIX cannot retrieve it for a non-child detached process).
    assert(status.contains(QStringLiteral("exitCode")));
    assert(status.value(QStringLiteral("exitCode")).toLongLong() == 137 &&
           "The crash fixture's distinctive exit code should be recovered on Windows");
#endif
  }
  std::cout << "  [PASS] Runtime crash is detected and (on Windows) its exit code recovered\n";

  // Test 7: a runtime host that fails fast (e.g. a module with no usable
  // native extension) reports "failed" with a specific error rather than
  // being mistaken for a crash - it does reach a terminal status on its own.
  {
    auto content_dir = makeContentDir();
    RuntimeBridge bridge;
    assert(bridge.connect().ok);
    auto config = makeConfig(content_dir.path(), QStringLiteral("scenario-bad-module"));
    assert(bridge.launch(config).ok);

    const auto status = pollUntilState(bridge, {QStringLiteral("failed")}, 3000);
    assert(status.value(QStringLiteral("stateName")).toString() == QStringLiteral("failed"));
    assert(status.value(QStringLiteral("lastError")).toString().contains(
        QStringLiteral("Native extension")));
  }
  std::cout << "  [PASS] A module load failure is reported as 'failed' with a specific error\n";

  // Test 8: a session that takes longer to stop than the bridge's 500ms
  // stop() wait budget still eventually reaches "stopped" (stop() itself
  // just returns early with an in-progress message).
  {
    auto content_dir = makeContentDir();
    RuntimeBridge bridge;
    assert(bridge.connect().ok);
    auto config = makeConfig(content_dir.path(), QStringLiteral("scenario-slow-stop"));
    assert(bridge.launch(config).ok);
    pollUntilState(bridge, {QStringLiteral("running")}, 3000);

    const auto stop_result = bridge.stop();
    assert(stop_result.ok);
    assert(stop_result.title != QStringLiteral("Session stopped") &&
           "A slow-stopping session should not be reported as stopped before it actually is");

    const auto status = pollUntilState(bridge, {QStringLiteral("stopped")}, 3000);
    assert(status.value(QStringLiteral("stateName")).toString() == QStringLiteral("stopped") &&
           "The slow-stopping session should eventually reach 'stopped' on its own");
  }
  std::cout << "  [PASS] A slow stop still converges on 'stopped' without blocking the caller\n";

  // Test 9: multiple sequential launches on the same bridge instance produce
  // independent sessions (distinct pids), and the bridge tracks the current
  // one correctly across the sequence.
  {
    auto content_dir = makeContentDir();
    RuntimeBridge bridge;
    assert(bridge.connect().ok);

    auto config_a = makeConfig(content_dir.path(), QStringLiteral("scenario-normal"));
    assert(bridge.launch(config_a).ok);
    const auto status_a = pollUntilState(bridge, {QStringLiteral("running")}, 3000);
    assert(status_a.value(QStringLiteral("stateName")).toString() == QStringLiteral("running"));
    const auto pid_a = status_a.value(QStringLiteral("pid")).toLongLong();
    assert(bridge.stop().ok);

    auto config_b = makeConfig(content_dir.path(), QStringLiteral("scenario-normal"));
    assert(bridge.launch(config_b).ok);
    const auto status_b = pollUntilState(bridge, {QStringLiteral("running")}, 3000);
    assert(status_b.value(QStringLiteral("stateName")).toString() == QStringLiteral("running"));
    const auto pid_b = status_b.value(QStringLiteral("pid")).toLongLong();
    assert(bridge.stop().ok);

    assert(pid_a > 0 && pid_b > 0);
    assert(pid_a != pid_b && "Sequential launches must spawn independent processes/sessions");
  }
  std::cout << "  [PASS] Multiple sequential launches produce independent sessions\n";

  std::cout << "All RuntimeBridge tests passed!\n";
  return 0;
}
