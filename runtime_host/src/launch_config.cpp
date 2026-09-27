#include "launch_config.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "xenon/core/json.hpp"

namespace xenon::runtime_host {

bool LaunchConfig::load_from_file(const std::string& path, LaunchConfig& out,
                                  std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    error = "Could not open launch configuration file: " + path;
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();

  xenon::core::JsonValue root;
  if (!xenon::core::JsonValue::parse(buffer.str(), root, &error)) {
    error = "Launch configuration is not valid JSON: " + error;
    return false;
  }
  if (!root.is_object()) {
    error = "Launch configuration must be a JSON object";
    return false;
  }

  // Absent configVersion means a pre-versioning launcher build; treat that as
  // version 1 rather than rejecting it, since version 1 is also the current
  // schema. Validate before touching any other field so an incompatible
  // schema is rejected up front instead of failing confusingly partway
  // through parsing a field that changed meaning between versions.
  out.config_version = static_cast<int>(root.get_number("configVersion", 1));
  if (out.config_version < 1) {
    error = "Launch configuration has an invalid configVersion (" +
            std::to_string(out.config_version) + "); expected >= 1";
    return false;
  }
  if (out.config_version > LaunchConfig::kCurrentVersion) {
    error = "Launch configuration version " + std::to_string(out.config_version) +
            " is newer than this xenon_runtime_host build supports (max " +
            std::to_string(LaunchConfig::kCurrentVersion) +
            "). Rebuild/reinstall a matching runtime host.";
    return false;
  }

  out.session_id = root.get_string("sessionId");
  out.session_dir = root.get_string("sessionDir");
  if (out.session_dir.empty()) {
    error = "Launch configuration is missing required field 'sessionDir'";
    return false;
  }

  out.game_id = root.get_string("gameId");
  out.title = root.get_string("title");
  out.content_path = root.get_string("contentPath");
  if (out.content_path.empty()) {
    error = "Launch configuration is missing required field 'contentPath'";
    return false;
  }

  out.module_id = root.get_string("moduleId");
  out.module_name = root.get_string("moduleName");
  out.module_path = root.get_string("modulePath");
  out.module_version = root.get_string("moduleVersion");
  if (const auto* settings = root.find("moduleSettings")) {
    out.module_settings_json = settings->dump();
  }
  if (const auto* requirements = root.find("runtimeApiRequirements")) {
    out.runtime_api_requirements_json = requirements->dump();
  }
  out.native_extension_path = root.get_string("nativeExtensionPath");
  out.adaptive_observation_path = root.get_string("adaptiveObservationPath");

  out.profile_id = root.get_string("profileId");
  out.profile_name = root.get_string("profileName");
  out.region = root.get_string("region");
  // profileXuid is a decimal string, not a JSON number: a 64-bit XUID does
  // not fit exactly in JsonValue's double-based number representation (see
  // RuntimeBridge::launch()'s matching encoding).
  out.profile_xuid = 0;
  if (const auto xuid_text = root.get_string("profileXuid"); !xuid_text.empty()) {
    try {
      out.profile_xuid = std::stoull(xuid_text);
    } catch (const std::exception&) {
      error = "Launch configuration field 'profileXuid' is not a valid unsigned 64-bit integer: " +
              xuid_text;
      return false;
    }
  }

  out.renderer = root.get_string("renderer", "Automatic");
  out.shader_cache = root.get_bool("shaderCache", true);
  out.shader_cache_mode = root.get_string("shaderCacheMode", "Persistent");

  out.input_backend = root.get_string("inputBackend", "Automatic");
  out.input_preferred_device = root.get_string("inputPreferredDevice", "Automatic");
  out.input_deadzone = root.get_number("inputDeadzone", 0.10);
  out.input_rumble = root.get_bool("inputRumble", true);
  out.input_background = root.get_bool("inputBackground", false);
  out.input_module_api_version = static_cast<int>(root.get_number("inputModuleApiVersion", 0));
  out.input_profile_store_path = root.get_string("inputProfileStorePath");
  out.input_user_sources.clear();
  if (const auto* routes = root.find("inputUserSources")) {
    if (const auto* route_array = routes->as_array()) {
      for (const auto& route : *route_array) {
        InputUserSources parsed_route{};
        parsed_route.user_index = static_cast<std::uint32_t>(route.get_number("userIndex", 0));
        if (const auto* sources = route.find("sources")) {
          if (const auto* source_array = sources->as_array()) {
            for (const auto& source : *source_array) {
              if (source.is_string()) parsed_route.sources.push_back(source.as_string());
            }
          }
        }
        if (parsed_route.user_index < 4) out.input_user_sources.push_back(std::move(parsed_route));
      }
    }
  }

  out.audio_master_volume = root.get_number("audioMasterVolume", 1.0);
  out.audio_mute_unfocused = root.get_bool("audioMuteUnfocused", false);
  out.audio_latency_profile = root.get_string("audioLatencyProfile");
  out.log_verbose = root.get_bool("logVerbose", false);

  out.title_update_path = root.get_string("titleUpdatePath");
  out.dlc_root_path = root.get_string("dlcRootPath");

  out.dlc.clear();
  // find() (not get(), which returns a JsonValue by value) so dlc_field
  // points into `root` itself - a pointer taken from get()'s temporary
  // return value would dangle past the end of this if-statement's
  // initializer, before the loop body below ever runs.
  if (const auto* dlc_field = root.find("dlc")) {
    if (const auto* dlc_array = dlc_field->as_array()) {
      for (const auto& entry : *dlc_array) {
        DlcEntry dlc_entry;
        dlc_entry.type = entry.get_string("type");
        dlc_entry.path = entry.get_string("path");
        out.dlc.push_back(std::move(dlc_entry));
      }
    }
  }

  out.save_path = root.get_string("savePath");
  out.screenshots_path = root.get_string("screenshotsPath");
  out.offline = root.get_bool("offline", true);
  out.headless_mode = root.get_bool("headlessMode", false);

  return true;
}

}  // namespace xenon::runtime_host
