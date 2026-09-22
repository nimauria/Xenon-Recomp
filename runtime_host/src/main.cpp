// Generic Xenon runtime/game-host process entry point. See
// docs/runtime/RUNTIME_HOST.md for the full process/IPC contract this implements.
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
#include "presentation_host.hpp"
#include "status_writer.hpp"
#include "xenon/core/session.hpp"
#include "xenon/xam/content_graph.hpp"
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
  config.verbose_logging = launch.log_verbose;
  config.enable_export_diagnostics = true;

  // "None" is the one explicit, deliberate way to ask for a graphics-less
  // session (e.g. a dedicated/offscreen host); every other renderer string
  // is handed to XenonSession::init_gpu(), which either constructs that real
  // backend or fails initialization outright - it never silently falls back
  // to a Null backend for a real Play request. See docs/runtime/RUNTIME_SESSION.md.
  config.enable_graphics = launch.renderer != "None";
  config.graphics_backend = launch.renderer;

  // Same "None" convention on the input side; anything else (including an
  // empty/legacy launch config's default-constructed string) is treated as
  // "Automatic" by XenonSession::init_input().
  config.enable_input = launch.input_backend != "None";
  if (config.enable_input && !launch.input_backend.empty() &&
      launch.input_backend != "Automatic") {
    config.input_drivers = {launch.input_backend};
  }

  // Audio V1 is a real, always-on subsystem for Normal Play - there is no
  // "None" audio backend selector at the launch-config level (every retail
  // Xbox 360 title has audio), so this requests it whenever this is not an
  // explicit headless/test launch, and XenonSession::init_audio() fails
  // session initialization outright if the host audio backend cannot
  // actually be created (no silent Null fallback) - see Part 5.4/5.1 of the
  // Gracemeria readiness pass. Only launch.headless_mode may run with audio
  // disabled entirely.
  config.enable_audio = !launch.headless_mode;
  config.audio_master_volume = static_cast<float>(launch.audio_master_volume);
  config.audio_mute_unfocused = launch.audio_mute_unfocused;
  config.audio_latency_profile = launch.audio_latency_profile;

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
    // A real audio/input backend failing to construct (see
    // init_audio()/init_input()) is the dominant real-world cause of session
    // init failure in Normal Play; graphics backend failure is caught
    // earlier still, inside initialize() itself, with the same category.
    // Distinguishing them precisely here would require XenonSession to
    // surface which subsystem failed structurally, which it does not yet do
    // - init_result.message already names it in free text.
    auto category = xenon::runtime_host::LaunchFailureCategory::SessionInitFailed;
    if (init_result.message.find("audio") != std::string::npos) {
      category = xenon::runtime_host::LaunchFailureCategory::AudioBackendFailed;
    } else if (init_result.message.find("input") != std::string::npos ||
               init_result.message.find("Input") != std::string::npos) {
      category = xenon::runtime_host::LaunchFailureCategory::InputBackendFailed;
    }
    status.write_fatal("Session initialization failed: " + init_result.message, category);
    std::cerr << "[runtime_host] " << init_result.message << std::endl;
    return 1;
  }
  status.write(session, "Session initialized");

  const std::filesystem::path content_path(launch.content_path);
  const auto xex_path = find_default_xex(content_path);
  if (xex_path.empty()) {
    status.write_fatal("Could not locate a default.xex under the game content path",
                       xenon::runtime_host::LaunchFailureCategory::MissingRequiredContent);
    return 1;
  }

  std::vector<std::byte> xex_bytes;
  if (!read_file_bytes(xex_path, xex_bytes)) {
    status.write_fatal("Could not read the XEX executable file: " + xex_path.string(),
                       xenon::runtime_host::LaunchFailureCategory::MissingRequiredContent);
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

  // Normal Play (the default - see LaunchConfig::headless_mode) requires the
  // title's base content to actually mount: a game whose game:/ device never
  // got mounted would otherwise run straight into an XEX Loader V2 that
  // loads fine but a filesystem that has nothing behind it, surfacing as a
  // confusing, much-later NtCreateFile failure instead of the real cause.
  // Only an explicit headless/test launch may continue without it (Part 5.2
  // of the Gracemeria readiness pass).
  if (title_id != 0) {
    auto mount_result = session.mount_content_graph(
        title_id, content_path,
        launch.title_update_path.empty() ? std::filesystem::path{}
                                         : std::filesystem::path(launch.title_update_path),
        launch.dlc_root_path.empty() ? std::filesystem::path{}
                                     : std::filesystem::path(launch.dlc_root_path),
        launch.profile_xuid);
    if (!mount_result.success) {
      if (!launch.headless_mode) {
        std::cerr << "[runtime_host] " << mount_result.message << std::endl;
        status.write_fatal("Required base content could not be mounted: " + mount_result.message,
                           xenon::runtime_host::LaunchFailureCategory::ContentMountFailed);
        return 1;
      }
      std::cout << "[runtime_host] Content graph mount failed (explicit headless mode; "
                   "continuing without it): "
                << mount_result.message << std::endl;
    }
  } else if (!launch.headless_mode) {
    const std::string message =
        "Could not determine the title ID from the XEX header; required base content cannot be "
        "resolved" +
        (parsed ? std::string() : (": " + parse_error));
    std::cerr << "[runtime_host] " << message << std::endl;
    status.write_fatal(message, xenon::runtime_host::LaunchFailureCategory::MissingRequiredContent);
    return 1;
  } else {
    std::cout << "[runtime_host] Skipping content graph mount (explicit headless mode): title ID "
                 "could not be determined"
              << std::endl;
  }
  status.write(session, "Content resolved");

  // Content Services (ContentManager::build_content_graph(), run above by
  // mount_content_graph()) is the one place a launch's title update is
  // discovered/selected - read its decision back here rather than
  // rediscovering a (potentially different) update independently. A
  // selected update whose file cannot be read is a hard launch failure
  // (never a silent fallback to the base XEX - see docs/runtime/RUNTIME_SESSION.md).
  std::vector<std::byte> title_update_bytes;
  if (const auto* content_graph = session.content_graph();
      content_graph != nullptr && content_graph->has_title_update()) {
    const auto& update_path = content_graph->selected_title_update->source_path;
    if (!read_file_bytes(update_path, title_update_bytes)) {
      status.write_fatal("Could not read the selected title-update file: " + update_path.string(),
                         xenon::runtime_host::LaunchFailureCategory::MissingRequiredContent);
      return 1;
    }
    std::cout << "[runtime_host] Selected title update: " << update_path.string() << std::endl;
  }

  auto load_result = session.load_game(xex_bytes, launch.game_id, title_update_bytes);
  status.write(session, load_result.success ? "Game loaded" : "Game load failed");
  if (!load_result.success) {
    std::cerr << "[runtime_host] " << load_result.message << std::endl;
    // A module whose native extension declares an incompatible effective-
    // executable revision (see XenonSession::load_native_extension()) fails
    // load_game() itself only when the failure happens to be treated as
    // fatal upstream; today it is instead a soft "not bound" diagnostic (see
    // below) that later fails at start() - tag the message-detectable case
    // here too so status.json is useful either way.
    auto category = xenon::runtime_host::LaunchFailureCategory::GameLoadFailed;
    if (load_result.message.find("revision") != std::string::npos ||
        load_result.message.find("title update") != std::string::npos) {
      category = xenon::runtime_host::LaunchFailureCategory::ModuleRevisionMismatch;
    }
    status.write_fatal(load_result.message, category);
    // Keep publishing status briefly so the launcher can read the specific
    // failure (missing native extension, unresolved imports, malformed/
    // incompatible title update, etc.) before this process exits.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return 1;
  }

  // Launch diagnostics: identify the pieces that determine what actually ran,
  // without leaking sensitive key material (see docs/runtime/RUNTIME_HOST.md).
  {
    const auto* loaded = session.loaded_xex();
    std::cout << "[runtime_host] module='" << launch.module_name << "' version='"
              << launch.module_version << "' game_id='" << launch.game_id << "'"
              << " title_id=0x" << std::hex << title_id << std::dec
              << " entry_point=0x" << std::hex
              << (loaded ? loaded->image.entry_point : 0u) << std::dec
              << " renderer='" << launch.renderer << "' (active="
              << (session.gpu() != nullptr ? "yes" : "no") << ")"
              << " audio_active=" << (session.audio() != nullptr ? "yes" : "no")
              << " input_backend='" << launch.input_backend << "'"
              << " (active=" << (session.input() != nullptr ? "yes" : "no") << ")"
              << std::endl;
    if (const auto& identity = session.effective_identity(); identity.has_value()) {
      std::cout << "[runtime_host] effective executable: title_update_applied="
                << (identity->title_update_applied ? "yes" : "no")
                << " base_version=0x" << std::hex << identity->base_version.value
                << " effective_version=0x" << identity->effective_version.value << std::dec
                << " hash=" << xenon::xbox::format_effective_image_hash(identity->effective_image_hash)
                << std::endl;
    }
    if (!session.native_extension_bound() && !session.native_extension_error().empty()) {
      std::cout << "[runtime_host] native extension not bound: " << session.native_extension_error()
                << std::endl;
    }
    if (launch.log_verbose) {
      for (const auto& unresolved : session.unresolved_imports()) {
        std::cout << "[runtime_host] unresolved import: " << unresolved.library << "!"
                  << unresolved.symbol << " (ordinal " << unresolved.ordinal << ")"
                  << std::endl;
      }
    }
  }

  // Presentation: the game window belongs to this process, not the launcher
  // (see docs/runtime/RUNTIME_HOST.md). Only created when a real graphics backend is
  // active - session.gpu() is null for a graphics-less session, and
  // NullBackend (explicit headless/test mode) has no presentation path
  // PresentationHost will accept (see its create()). Created BEFORE
  // session.start() so a window/surface failure is caught before any guest
  // code ever runs, instead of stopping an already-started game (Part 5.3 of
  // the Gracemeria readiness pass: a real graphics backend with no usable
  // window must never let Normal Play continue).
#if defined(XENON_HAS_PRESENTATION_HOST)
  xenon::runtime_host::PresentationHost presentation;
  bool presentation_active = false;
  if (session.gpu() != nullptr) {
    std::string presentation_error;
    presentation_active = presentation.create(1280, 720, launch.title.empty() ? "Xenon" : launch.title,
                                              *session.gpu(), &presentation_error);
    if (!presentation_active) {
      if (!launch.headless_mode) {
        std::cerr << "[runtime_host] " << presentation_error << std::endl;
        status.write_fatal("Presentation window/surface could not be created: " + presentation_error,
                           xenon::runtime_host::LaunchFailureCategory::WindowCreationFailed);
        return 1;
      }
      std::cout << "[runtime_host] Presentation window not created (explicit headless mode; "
                   "continuing without it): "
                << presentation_error << std::endl;
    } else {
      std::cout << "[runtime_host] Presentation window created (1280x720)" << std::endl;
    }
  }
#endif

  auto start_result = session.start();
  status.write(session, start_result.success ? "Running" : "Start failed");
  if (!start_result.success) {
    std::cerr << "[runtime_host] " << start_result.message << std::endl;
    status.write_fatal("Game execution failed to start: " + start_result.message,
                       xenon::runtime_host::LaunchFailureCategory::StartFailed);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return 1;
  }

  // Supervise: publish status regularly, watch for a stop request, and give
  // cooperative shutdown a bounded grace period before hard-exiting. There
  // is no way to preempt already-running native compiled code, so a game
  // that never checks XenonSession::stop_requested() (via a future host
  // hook) can only be stopped by ending this process outright.
  constexpr auto kPollInterval = std::chrono::milliseconds(16);
  constexpr auto kStatusInterval = std::chrono::milliseconds(250);
  constexpr auto kStopGracePeriod = std::chrono::seconds(5);
  bool stop_sent = false;
  std::optional<std::chrono::steady_clock::time_point> stop_deadline;
  auto next_status_write = std::chrono::steady_clock::now();

  while (true) {
    const bool active = session.is_running() || session.execution_active();
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_status_write) {
      status.write(session);
      next_status_write = now + kStatusInterval;
    }

    if (!active) break;

#if defined(XENON_HAS_PRESENTATION_HOST)
    if (presentation_active) {
      xenon::runtime_host::PresentationEvents events{};
      presentation.pump_events(events);
      if (events.close_requested && !stop_sent) {
        // Window close requests an orderly session shutdown, matching
        // real Xbox 360 dashboard/game-close behavior - it does not
        // terminate this process directly (the hard-stop grace-period
        // path below still applies if the game does not cooperate).
        std::cout << "[runtime_host] Window close requested" << std::endl;
        const auto stop_result = session.stop();
        std::cout << "[runtime_host] " << stop_result.message << std::endl;
        stop_sent = true;
        stop_deadline = std::chrono::steady_clock::now() + kStopGracePeriod;
      }
      if (events.focus_changed) {
        session.set_focused(events.focused);
      }
      if (events.resized) {
        std::cout << "[runtime_host] Presentation resized to " << events.width << "x"
                  << events.height << std::endl;
      }
    }
#endif

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

#if defined(XENON_HAS_PRESENTATION_HOST)
  presentation.destroy();
#endif

  status.write(session, "Session ended");
  std::cout << "[runtime_host] Session ended: " << session.last_error() << std::endl;
  return session.state() == xenon::core::SessionState::Failed ? 1 : 0;
}
