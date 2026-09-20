// Lightweight stand-in for xenon_runtime_host, used only by
// launcher/tests/runtime/runtime_bridge_tests.cpp. It speaks the same
// file-based contract documented in docs/RUNTIME_HOST.md (launch-config.json
// in; status.json/log.txt out; stop.signal requests a stop) without ever
// touching XenonSession, so RuntimeBridge's process-supervision behavior can
// be exercised deterministically and without real game content, a native
// extension, or CPU V2 compiled code. See RuntimeBridge::connect(), which
// honors XENON_RUNTIME_HOST_PATH specifically so tests can point it here.
//
// The scenario to run is selected by the launch config's "gameId" field
// (fully test-controlled, never a real title identifier in this fixture):
//
//   scenario-normal              initializing -> ready -> loading -> running,
//                                 then stops within the bridge's 500ms
//                                 stop() wait window once stop.signal appears
//   scenario-bad-module          reaches "ready" then fails fast with a
//                                 native-extension-style error, like the real
//                                 host's load_game() failure path
//   scenario-crash                writes one "running" status, then exits
//                                 abruptly (std::_Exit) without ever writing
//                                 a terminal status - never sees stop.signal
//   scenario-slow-stop            like scenario-normal, but takes longer to
//                                 honor stop.signal than the bridge's 500ms
//                                 stop() wait window (still stops eventually)
//   (anything else)               falls back to scenario-normal
//
// This is intentionally not xenon_runtime_host with extra flags: it must
// stay independent of XenonSession/xenon_core so a bug in the real session
// can never make the test double lie about the contract.

#include "xenon/core/json.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

using xenon::core::JsonValue;

struct ParsedLaunchConfig {
  std::string session_id;
  std::string session_dir;
  std::string game_id;
  std::string title;
};

std::optional<std::string> parse_launch_config_argument(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    constexpr std::string_view kFlag = "--launch-config";
    if (arg.rfind(kFlag, 0) == 0) {
      if (arg.size() > kFlag.size() && arg[kFlag.size()] == '=') {
        return arg.substr(kFlag.size() + 1);
      }
      if (i + 1 < argc) return std::string(argv[i + 1]);
    }
  }
  return std::nullopt;
}

bool load_launch_config(const std::string& path, ParsedLaunchConfig& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return false;
  std::ostringstream buffer;
  buffer << file.rdbuf();

  JsonValue root;
  std::string error;
  if (!JsonValue::parse(buffer.str(), root, &error) || !root.is_object()) return false;

  out.session_id = root.get_string("sessionId");
  out.session_dir = root.get_string("sessionDir");
  out.game_id = root.get_string("gameId");
  out.title = root.get_string("title");
  return !out.session_dir.empty();
}

std::uint64_t current_process_id() {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::int64_t now_epoch_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

class FixtureStatusWriter {
 public:
  explicit FixtureStatusWriter(const ParsedLaunchConfig& config)
      : session_dir_(config.session_dir),
        session_id_(config.session_id),
        game_id_(config.game_id),
        title_(config.title),
        started_at_epoch_ms_(now_epoch_ms()) {
    std::error_code ec;
    std::filesystem::create_directories(session_dir_, ec);
  }

  // `state`/`state_name` mirror xenon::core::SessionState's numbering (see
  // status_writer.cpp's real state_name()), so a test asserting on either
  // representation behaves the same against the fixture as against the real
  // runtime host.
  void write(int state, std::string_view state_name, bool running, bool execution_active,
             std::string_view last_error = {}) {
    JsonValue root = JsonValue::make_object();
    root.set("available", true);
    root.set("pid", static_cast<double>(current_process_id()));
    root.set("sessionId", session_id_);
    root.set("gameId", game_id_);
    root.set("title", title_);
    root.set("state", static_cast<double>(state));
    root.set("stateName", std::string(state_name));
    root.set("initialized", state >= 2);
    root.set("running", running);
    root.set("executionActive", execution_active);
    root.set("lastError", std::string(last_error));
    root.set("startedAtEpochMs", static_cast<double>(started_at_epoch_ms_));
    root.set("updatedAtEpochMs", static_cast<double>(now_epoch_ms()));

    const auto final_path = std::filesystem::path(session_dir_) / "status.json";
    const auto temp_path = final_path.string() + ".tmp";
    {
      std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
      out << root.dump();
    }
    std::error_code rename_error;
    std::filesystem::rename(temp_path, final_path, rename_error);
  }

  [[nodiscard]] bool stop_requested() const {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(session_dir_) / "stop.signal", ec);
  }

  void append_log(std::string_view line) const {
    std::ofstream log(std::filesystem::path(session_dir_) / "log.txt",
                      std::ios::binary | std::ios::app);
    log << line << "\n";
  }

 private:
  std::string session_dir_;
  std::string session_id_;
  std::string game_id_;
  std::string title_;
  std::int64_t started_at_epoch_ms_;
};

// SessionState numbering (docs/RUNTIME_SESSION.md / include/xenon/core/session.hpp):
// Uninitialized=0 Initializing=1 Ready=2 LoadingGame=3 Running=4 Paused=5
// Stopping=6 Stopped=7 Failed=8
constexpr int kInitializing = 1;
constexpr int kReady = 2;
constexpr int kLoadingGame = 3;
constexpr int kRunning = 4;
constexpr int kStopping = 6;
constexpr int kStopped = 7;
constexpr int kFailed = 8;

int run_normal_scenario(FixtureStatusWriter& status, std::chrono::milliseconds stop_delay) {
  status.write(kInitializing, "initializing", false, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  status.write(kReady, "ready", false, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  status.write(kLoadingGame, "loading", false, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  status.write(kRunning, "running", true, true);
  status.append_log("[fixture] Running");

  constexpr auto kPollInterval = std::chrono::milliseconds(30);
  // Safety bound so a test bug (never sending stop.signal) cannot hang a
  // test run forever.
  constexpr auto kMaxLifetime = std::chrono::seconds(20);
  const auto deadline = std::chrono::steady_clock::now() + kMaxLifetime;
  bool stopping = false;
  std::optional<std::chrono::steady_clock::time_point> stop_at;

  while (std::chrono::steady_clock::now() < deadline) {
    if (!stopping && status.stop_requested()) {
      stopping = true;
      status.write(kStopping, "stopping", true, true);
      status.append_log("[fixture] Stop requested");
      stop_at = std::chrono::steady_clock::now() + stop_delay;
    }
    if (stop_at && std::chrono::steady_clock::now() >= *stop_at) {
      status.write(kStopped, "stopped", false, false);
      status.append_log("[fixture] Stopped");
      return 0;
    }
    if (!stopping) status.write(kRunning, "running", true, true);
    std::this_thread::sleep_for(kPollInterval);
  }
  status.write(kStopped, "stopped", false, false);
  return 0;
}

int run_bad_module_scenario(FixtureStatusWriter& status) {
  status.write(kInitializing, "initializing", false, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  status.write(kReady, "ready", false, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  status.write(kFailed, "failed", false, false,
               "Native extension not found: could not bind the module's compiled-code registry");
  status.append_log("[fixture] Game load failed: missing native extension");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return 1;
}

int run_crash_scenario(FixtureStatusWriter& status) {
  status.write(kInitializing, "initializing", false, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  status.write(kRunning, "running", true, true);
  status.append_log("[fixture] Running (about to crash)");
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  // Deliberately never writes a terminal status and exits with a
  // distinctive, non-zero code - this is what RuntimeBridge::augmentedStatus()
  // must recognize as a crash (see docs/RUNTIME_HOST.md "Detecting a crash").
  std::_Exit(137);
}

}  // namespace

int main(int argc, char** argv) {
  const auto config_path = parse_launch_config_argument(argc, argv);
  if (!config_path) {
    std::cerr << "runtime_host_fixture: missing --launch-config <path>\n";
    return 2;
  }

  ParsedLaunchConfig config{};
  if (!load_launch_config(*config_path, config)) {
    std::cerr << "runtime_host_fixture: could not read/parse launch config: " << *config_path
              << "\n";
    return 2;
  }

  FixtureStatusWriter status(config);
  status.append_log("[fixture] Starting scenario '" + config.game_id + "'");

  if (config.game_id == "scenario-bad-module") {
    return run_bad_module_scenario(status);
  }
  if (config.game_id == "scenario-crash") {
    return run_crash_scenario(status);
  }
  if (config.game_id == "scenario-slow-stop") {
    return run_normal_scenario(status, std::chrono::milliseconds(900));
  }
  return run_normal_scenario(status, std::chrono::milliseconds(60));
}
