#include "host_setup.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "content_source.hpp"
#include "launch_config.hpp"
#include "presentation_host.hpp"
#include "status_writer.hpp"
#include "xenon/core/session.hpp"
#if defined(XENON_RUNTIME_HAS_MEMORY) && XENON_RUNTIME_HAS_MEMORY
#include "xenon/memory/host_vm.hpp"
#include "xenon/memory/types.hpp"
#endif
#include "xenon/xam/content_graph.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::runtime_host {

#ifndef XENON_HOST_PLATFORM_NAME
#define XENON_HOST_PLATFORM_NAME "unknown"
#endif
#ifndef XENON_HOST_ARCH_NAME
#define XENON_HOST_ARCH_NAME "unknown"
#endif

void log_host_capabilities() {
  std::cout << "[runtime_host] Host platform=" << XENON_HOST_PLATFORM_NAME
            << " arch=" << XENON_HOST_ARCH_NAME
            << " pointer_bits=" << (sizeof(void*) * 8u) << std::endl;
#if defined(XENON_RUNTIME_HAS_MEMORY) && XENON_RUNTIME_HAS_MEMORY
  const auto capabilities = xenon::memory::host_vm::capabilities();
  const bool xbox_4k_aperture =
      sizeof(void*) >= 8u && capabilities.supports_fixed_mapping_granularity(
                                  xenon::memory::kBasePageSize);
  std::cout << "[runtime_host] Host VM page_size=" << capabilities.page_size
            << " allocation_granularity=" << capabilities.allocation_granularity
            << " fixed_shared_mapping="
            << (capabilities.fixed_shared_mapping ? "yes" : "no")
            << " fixed_mapping_granularity="
            << capabilities.fixed_shared_mapping_granularity
            << " page_views="
            << (capabilities.fixed_shared_mapping_requires_page_views ? "yes"
                                                                       : "no")
            << " xbox_4k_aperture_compatible="
            << (xbox_4k_aperture ? "yes" : "no") << std::endl;
#endif
}

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
#else
  std::freopen(log_path.c_str(), "a", stdout);
#endif
  // Two real, confirmed bugs traced to here, both from giving stdout and
  // stderr independent freopen()s of the same path:
  //
  // 1. Silent data loss: two separate FILE objects opened "a" on the same
  //    path each cache their own idea of end-of-file and don't re-query it
  //    on every write, so whichever stream flushes second overwrites bytes
  //    the other just wrote instead of appending after them - reproduced
  //    standalone (freopen both, write cout/cerr/cout, read the file back:
  //    the cerr line is simply gone, no corruption, no error). Every
  //    std::cerr line this process ever wrote to the log was at risk of
  //    silently vanishing this way, including the ones diagnosing a fatal
  //    startup failure - exactly the messages a log file exists to keep.
  // 2. A real crash: the UCRT determines a freopen'd stream's buffering
  //    mode (console vs. file) lazily, on that stream's first write, by
  //    probing the underlying handle. Confirmed via cdb: main()'s very
  //    first std::cerr write (the "Start failed" diagnostic, itself
  //    already holding XenonSession::console_log_mutex(), which forwards
  //    to logging::Logger::stream_mutex() - see session.hpp - with no
  //    other writer active) crashed with STATUS_STACK_BUFFER_OVERRUN
  //    (debug-CRT invalid_parameter fast-fail) inside ucrtbased!write,
  //    called from that first cerr flush. A mutex serializes writers
  //    against each other; it cannot make one stream's own first-write
  //    probe safe.
  //
  // Fix for (1): give stderr no FILE object of its own - alias its fd onto
  // stdout's already-open handle, so both streams share one OS-level file
  // position instead of two independently cached ones (reproduced fixed
  // standalone with the same repro above: nothing goes missing).
  // Fix for (2): set each stream's buffering mode explicitly, immediately
  // after it starts pointing at the log file and before any write can
  // reach it, so the racy lazy probe never runs at all.
#if defined(_WIN32)
  _dup2(_fileno(stdout), _fileno(stderr));
#else
  dup2(fileno(stdout), fileno(stderr));
#endif
  std::setvbuf(stdout, nullptr, _IOFBF, BUFSIZ);
  std::setvbuf(stderr, nullptr, _IOFBF, BUFSIZ);
  std::ios::sync_with_stdio(true);
}

bool read_host_file_bytes(const std::filesystem::path& path, std::vector<std::byte>& out_bytes) {
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
  config.memory_watch_addresses = launch.memory_watch_addresses;
  config.memory_watch_history = launch.memory_watch_history;
  config.memory_watch_poll_ms = launch.memory_watch_poll_ms;
  config.enable_export_diagnostics = true;
  config.enable_export_trace = true;

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
  config.input_preferred_device = launch.input_preferred_device;
  config.input_deadzone = static_cast<float>(launch.input_deadzone);
  config.input_rumble = launch.input_rumble;
  config.input_background = launch.input_background;
  config.input_profile_store_path = launch.input_profile_store_path;
  for (const auto& route : launch.input_user_sources) {
    if (route.user_index < config.input_user_sources.size()) {
      config.input_user_sources[route.user_index] = route.sources;
    }
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
  if (!launch.adaptive_observation_path.empty())
    config.adaptive_observation_path = launch.adaptive_observation_path;
  if (!launch.session_dir.empty()) {
    config.adaptive_observation_mirror_path =
        std::filesystem::path(launch.session_dir) / "adaptive-observations.jsonl";
  }
  return config;
}

}  // namespace xenon::runtime_host
