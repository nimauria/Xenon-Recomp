#pragma once

// Launch configuration contract for the Xenon runtime host. See
// docs/runtime/RUNTIME_HOST.md for the authoritative schema. The launcher writes one
// of these as JSON per Play action; this process reads it once at startup.

#include <cstdint>
#include <string>
#include <vector>

namespace xenon::runtime_host {

struct DlcEntry {
  std::string type;
  std::string path;
};

struct InputUserSources {
  std::uint32_t user_index{0};
  std::vector<std::string> sources{};
};

struct LaunchConfig {
  // Schema version of the launch configuration contract this struct/parser
  // implements. Bump this and reject older/newer configs in
  // load_from_file() whenever a field's meaning changes incompatibly (adding
  // an optional field with a safe default does not require a bump). See
  // docs/runtime/RUNTIME_HOST.md.
  static constexpr int kCurrentVersion = 1;

  // Defaults to kCurrentVersion so a LaunchConfig built directly in code
  // (rather than parsed from JSON) is always self-consistent.
  int config_version = kCurrentVersion;

  std::string session_id;
  // Directory this process writes status.json/log.txt into and watches for
  // stop.signal. Owned by the launcher; created before this process starts.
  std::string session_dir;

  std::string game_id;
  std::string title;
  std::string content_path;

  std::string module_id;
  std::string module_name;
  std::string module_path;
  std::string module_version;
  // JSON snapshots of generic launcher/module configuration. The runtime
  // preserves these across the process boundary even when a particular
  // subsystem has no consumer yet, so settings do not silently disappear.
  std::string module_settings_json{"{}"};
  std::string runtime_api_requirements_json{"{}"};

  // Absolute path to the module's compiled-code native extension library, or
  // empty if the module supplies none (session still loads; it just cannot
  // run - see XenonSession::native_extension_bound()).
  std::string native_extension_path;

  std::string profile_id;
  std::string profile_name;
  std::string region;
  std::uint64_t profile_xuid{0};

  std::string renderer;
  bool shader_cache{true};
  std::string shader_cache_mode{"Persistent"};

  std::string input_backend;
  std::string input_preferred_device{"Automatic"};
  double input_deadzone{0.10};
  bool input_rumble{true};
  bool input_background{false};
  int input_module_api_version{0};
  std::string input_profile_store_path;
  std::vector<InputUserSources> input_user_sources{};

  double audio_master_volume{1.0};
  bool audio_mute_unfocused{false};
  std::string audio_latency_profile;

  // Mirrors the launcher's live "developer/verboseLogging" preference.
  bool log_verbose{false};

  std::string title_update_path;
  std::string dlc_root_path;
  std::vector<DlcEntry> dlc;

  std::string save_path;
  std::string screenshots_path;
  bool offline{true};

  // Explicit, opt-in headless/test/developer mode (Part 5 of the Gracemeria
  // readiness pass - see docs/runtime/RUNTIME_HOST.md's "Normal Play vs headless"
  // section). Defaults to false: an ordinary launcher-issued Play action is
  // always "Normal Play", where required base content, a requested real
  // presentation window/surface, and audio must all actually succeed or the
  // launch fails outright (see main.cpp) - never a silent "continuing
  // without it". Only a caller that explicitly sets this may run with
  // content/presentation/audio missing, e.g. an automated compatibility
  // sweep or a dedicated/offscreen host with no window.
  bool headless_mode{false};

  // Parses a launch configuration from a JSON file at `path`. Returns false
  // and fills `error` on any structural problem (missing file, invalid JSON,
  // missing required fields).
  [[nodiscard]] static bool load_from_file(const std::string& path, LaunchConfig& out,
                                           std::string& error);
};

}  // namespace xenon::runtime_host
