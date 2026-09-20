#pragma once

// Launch configuration contract for the Xenon runtime host. See
// docs/RUNTIME_HOST.md for the authoritative schema. The launcher writes one
// of these as JSON per Play action; this process reads it once at startup.

#include <cstdint>
#include <string>
#include <vector>

namespace xenon::runtime_host {

struct DlcEntry {
  std::string type;
  std::string path;
};

struct LaunchConfig {
  // Schema version of the launch configuration contract this struct/parser
  // implements. Bump this and reject older/newer configs in
  // load_from_file() whenever a field's meaning changes incompatibly (adding
  // an optional field with a safe default does not require a bump). See
  // docs/RUNTIME_HOST.md.
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

  // Absolute path to the module's compiled-code native extension library, or
  // empty if the module supplies none (session still loads; it just cannot
  // run - see XenonSession::native_extension_bound()).
  std::string native_extension_path;

  std::string profile_id;
  std::string profile_name;
  std::string region;
  std::uint64_t profile_xuid{0};

  std::string renderer;
  std::string input_backend;

  double audio_master_volume{1.0};
  bool audio_mute_unfocused{false};
  std::string audio_latency_profile;

  // Mirrors the launcher's "developer/verboseLogging" preference. Defaults
  // to false; RuntimeBridge does not yet have access to SettingsService to
  // forward the live value (see RuntimeBridge::launch()), so this is
  // currently always written as false - a real, parsed field the runtime
  // host already honors (see build_session_config() in main.cpp), not a
  // stub, but not yet fed a live setting either.
  bool log_verbose{false};

  std::string title_update_path;
  std::string dlc_root_path;
  std::vector<DlcEntry> dlc;

  std::string save_path;
  std::string screenshots_path;
  bool offline{true};

  // Parses a launch configuration from a JSON file at `path`. Returns false
  // and fills `error` on any structural problem (missing file, invalid JSON,
  // missing required fields).
  [[nodiscard]] static bool load_from_file(const std::string& path, LaunchConfig& out,
                                           std::string& error);
};

}  // namespace xenon::runtime_host
