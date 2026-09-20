// Generic Xenon runtime/game-host process entry point. See
// docs/RUNTIME_HOST.md for the full process/IPC contract this implements.
//
// This process is the only thing that ever creates and owns a XenonSession.
// The launcher never executes guest code itself (see
// docs/architecture/PROJECT_STRUCTURE.md); it spawns one of these per Play
// action, hands it a launch configuration, and supervises it entirely
// through files in the session directory (status.json out, stop.signal in).
// Running detached from the launcher process is what makes "the game
// continues if the launcher exits" true: there is no parent/child
// relationship the OS would tear down together.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "launch_config.hpp"
#include "status_writer.hpp"
#include "xenon/core/session.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace {

using xenon::runtime_host::LaunchConfig;
using xenon::runtime_host::StatusWriter;

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

// Redirects stdout/stderr into the session log file so every
// XenonSession/runtime-host diagnostic line lands somewhere the launcher can
// tail, since this process normally runs detached with no visible console.
void redirect_log(const std::string& session_dir) {
  std::error_code error;
  std::filesystem::create_directories(session_dir, error);
  const auto log_path = (std::filesystem::path(session_dir) / "log.txt").string();
#if defined(_WIN32)
  FILE* out = nullptr;
  freopen_s(&out, log_path.c_str(), "a", stdout);
  FILE* err = nullptr;
  freopen_s(&err, log_path.c_str(), "a", stderr);
#else
  std::freopen(log_path.c_str(), "a", stdout);
  std::freopen(log_path.c_str(), "a", stderr);
#endif
  std::ios::sync_with_stdio(true);
}

// Searches common default.xex spellings directly under content_path. Real
// disc image / STFS package resolution happens earlier, in the launcher's
// content probe; by the time this process runs, content_path is expected to
// already be an extracted/root game directory containing one.
std::filesystem::path find_default_xex(const std::filesystem::path& content_path) {
  for (const auto* name : {"default.xex", "Default.xex", "DEFAULT.XEX"}) {
    auto candidate = content_path / name;
    std::error_code error;
    if (std::filesystem::exists(candidate, error)) return candidate;
  }
  return {};
}

bool read_file_bytes(const std::filesystem::path& path, std::vector<std::byte>& out_bytes) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) return false;
  const auto size = file.tellg();
  if (size <= 0 || size > 512 * 1024 * 1024) return false;
  out_bytes.resize(static_cast<std::size_t>(size));
  file.seekg(0, std::ios::beg);
  file.read(reinterpret_cast<char*>(out_bytes.data()), size);
  return file.good();
}

xenon::core::SessionConfig build_session_config(const LaunchConfig& launch) {
  xenon::core::SessionConfig config{};
  config.enable_logging = true;
  config.enable_export_diagnostics = true;
  // Real backend selection is not implemented yet (XenonSession::init_gpu()
  // always creates a NullBackend today - see docs/RUNTIME_SESSION.md). The
  // request is still recorded so status/UI can show it against the actual
  // (currently always "null") active backend rather than hiding the gap.
  config.enable_graphics = launch.renderer != "None";
  config.graphics_backend = launch.renderer;
  config.enable_input = true;
  config.native_extension_path = launch.native_extension_path;
  return config;
}

}  // namespace

int main(int argc, char** argv) {
  const auto config_path = parse_launch_config_argument(argc, argv);
  if (!config_path) {
    std::cerr << "xenon_runtime_host: missing --launch-config <path>\n";
    return 2;
  }

  LaunchConfig launch{};
  std::string load_error;
  if (!LaunchConfig::load_from_file(*config_path, launch, load_error)) {
    std::cerr << "xenon_runtime_host: " << load_error << "\n";
    return 2;
  }

  redirect_log(launch.session_dir);
  std::cout << "[runtime_host] Starting session '" << launch.session_id << "' for game '"
            << launch.game_id << "' (" << launch.title << ")" << std::endl;

  StatusWriter status(launch.session_dir, launch);

  xenon::core::XenonSession session;
  auto init_result = session.initialize(build_session_config(launch));
  if (!init_result.success) {
    status.write_fatal("Session initialization failed: " + init_result.message);
    std::cerr << "[runtime_host] " << init_result.message << std::endl;
    return 1;
  }
  status.write(session, "Session initialized");

  const std::filesystem::path content_path(launch.content_path);
  const auto xex_path = find_default_xex(content_path);
  if (xex_path.empty()) {
    status.write_fatal("Could not locate a default.xex under the game content path");
    return 1;
  }

  std::vector<std::byte> xex_bytes;
  if (!read_file_bytes(xex_path, xex_bytes)) {
    status.write_fatal("Could not read the XEX executable file: " + xex_path.string());
    return 1;
  }

  // Peek the XEX header for the title ID before loading, so content mounting
  // (game:/dlcN:/saves: VFS devices) happens with the real title identity
  // instead of the placeholder "always 0" the launcher used previously.
  xenon::xbox::XexImage probed_image{};
  std::string parse_error;
  const bool parsed = xenon::xbox::parse_xex_image(xex_bytes, probed_image, &parse_error);
  const std::uint32_t title_id = parsed ? probed_image.title_id : 0;
  if (!parsed) {
    std::cout << "[runtime_host] Could not read XEX header for content identity: " << parse_error
              << std::endl;
  }

  if (title_id != 0) {
    auto mount_result = session.mount_content_graph(
        title_id, content_path,
        launch.title_update_path.empty() ? std::filesystem::path{}
                                         : std::filesystem::path(launch.title_update_path),
        launch.dlc_root_path.empty() ? std::filesystem::path{}
                                     : std::filesystem::path(launch.dlc_root_path),
        launch.profile_xuid);
    if (!mount_result.success) {
      std::cout << "[runtime_host] Content graph mount failed (continuing without it): "
                << mount_result.message << std::endl;
    }
  } else {
    std::cout << "[runtime_host] Skipping content graph mount: title ID could not be determined"
              << std::endl;
  }
  status.write(session, "Content resolved");

  auto load_result = session.load_game(xex_bytes, launch.game_id);
  status.write(session, load_result.success ? "Game loaded" : "Game load failed");
  if (!load_result.success) {
    std::cerr << "[runtime_host] " << load_result.message << std::endl;
    // Keep publishing status briefly so the launcher can read the specific
    // failure (missing native extension, unresolved imports, etc.) before
    // this process exits.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return 1;
  }

  auto start_result = session.start();
  status.write(session, start_result.success ? "Running" : "Start failed");
  if (!start_result.success) {
    std::cerr << "[runtime_host] " << start_result.message << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return 1;
  }

  // Supervise: publish status regularly, watch for a stop request, and give
  // cooperative shutdown a bounded grace period before hard-exiting. There
  // is no way to preempt already-running native compiled code, so a game
  // that never checks XenonSession::stop_requested() (via a future host
  // hook) can only be stopped by ending this process outright.
  constexpr auto kPollInterval = std::chrono::milliseconds(250);
  constexpr auto kStopGracePeriod = std::chrono::seconds(5);
  bool stop_sent = false;
  std::optional<std::chrono::steady_clock::time_point> stop_deadline;

  while (true) {
    const bool active = session.is_running() || session.execution_active();
    status.write(session);

    if (!active) break;

    if (!stop_sent && status.stop_requested()) {
      std::cout << "[runtime_host] Stop requested by launcher" << std::endl;
      const auto stop_result = session.stop();
      std::cout << "[runtime_host] " << stop_result.message << std::endl;
      stop_sent = true;
      stop_deadline = std::chrono::steady_clock::now() + kStopGracePeriod;
    }

    if (stop_deadline && std::chrono::steady_clock::now() >= *stop_deadline) {
      std::cout << "[runtime_host] Guest execution did not stop cooperatively within "
                << kStopGracePeriod.count() << "s; terminating" << std::endl;
      status.write_fatal("Stopped: game did not exit cooperatively and was terminated");
      std::fflush(stdout);
      std::_Exit(0);
    }

    std::this_thread::sleep_for(kPollInterval);
  }

  status.write(session, "Session ended");
  std::cout << "[runtime_host] Session ended: " << session.last_error() << std::endl;
  return session.state() == xenon::core::SessionState::Failed ? 1 : 0;
}
