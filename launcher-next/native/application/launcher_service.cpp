#include "launcher_service.hpp"
#include "core/archive.hpp"
#include "core/module_settings.hpp"
#include "core/module_presentation.hpp"
#include "core/launcher_paths.hpp"
#include "core/profile_names.hpp"
#include "core/cache_storage.hpp"
#include "core/module_manifest.hpp"
#include "core/releases.hpp"
#include "core/settings.hpp"
#include "core/support_bundle.hpp"
#include "core/dlc_catalog.hpp"
#include "core/game_media.hpp"
#include "core/game_activity.hpp"
#include "catalogues.hpp"
#include "platform/desktop.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <random>
#include <map>
#include <set>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
std::string uuid() { return make_id(); }
std::string now() {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}
std::string trim(std::string value) {
  auto start = value.find_first_not_of(" \t\r\n");
  return start == std::string::npos
             ? ""
             : value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
}
std::string lower(std::string value) {
  for (auto &c : value)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return value;
}
Json read_json(const fs::path &path) { return read_document(path); }
void atomic_json(const fs::path &path, const Json &value) { write_document(path, value); }
Json &item(Json &list, const std::string &id) {
  auto found = std::find_if(list.begin(), list.end(),
                            [&](const auto &row) { return row.value("id", "") == id; });
  if (found == list.end())
    throw std::runtime_error("Item no longer exists");
  return *found;
}
const Json &item(const Json &list, const std::string &id) {
  const auto found = std::find_if(list.begin(), list.end(),
                                  [&](const auto &row) { return row.value("id", "") == id; });
  if (found == list.end())
    throw std::runtime_error("Item no longer exists");
  return *found;
}
void module_metadata(Json &game, const Json &module, std::int64_t revision, bool title = false,
                     const Json *catalog = nullptr) {
  apply_module_presentation(game, module, catalog, revision, title);
}
void remove_item(Json &list, const std::string &id) {
  (void)item(list, id);
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const auto &row) { return row.value("id", "") == id; }),
             list.end());
}
Json manifest(const fs::path &folder) {
  for (const auto *name : {"xenon-module.json", "module.json", "manifest.json"})
    if (fs::is_regular_file(folder / name))
      return read_json(folder / name);
  throw std::runtime_error("No Xenon module manifest found in that folder");
}
std::string name_prefix(const std::string &name, std::size_t maximum) {
  std::size_t offset = 0, units = 0;
  while (offset < name.size()) {
    const auto first = static_cast<unsigned char>(name[offset]);
    const std::size_t bytes = first < 0x80 ? 1 : first < 0xE0 ? 2 : first < 0xF0 ? 3 : 4;
    const auto next = units + (bytes == 4 ? 2 : 1);
    if (next > maximum || offset + bytes > name.size())
      break;
    offset += bytes;
    units = next;
  }
  return name.substr(0, offset);
}
std::string unique_profile_name(const Json &profiles, const std::string &base) {
  auto candidate = name_prefix(base, 48);
  for (unsigned suffix = 2;
       std::any_of(profiles.begin(), profiles.end(),
                   [&](const auto &row) {
                     return profile_name_key(row.value("name", "")) == profile_name_key(candidate);
                   });
       ++suffix) {
    const auto ending = " (" + std::to_string(suffix) + ")";
    candidate = name_prefix(base, 48 - ending.size()) + ending;
  }
  return candidate;
}
void validate_name(const Json &profiles, const std::string &name, const std::string &exclude = "",
                   bool retained_name = false) {
  if (name.empty() || name_prefix(name, 48) != name)
    throw std::runtime_error("Enter a profile name of at most 48 characters");
  const auto key = profile_name_key(name);
  if (retained_name)
    return;
  for (const auto &profile : profiles)
    if (profile.at("id") != exclude &&
        profile_name_key(profile.at("name").get<std::string>()) == key)
      throw std::runtime_error("That profile name is already in use");
}
} // namespace
std::string path_string(const fs::path &path) {
  const auto utf8 = path.u8string();
  return {utf8.begin(), utf8.end()};
}
fs::path default_storage() {
#ifdef _WIN32
  if (const auto *root = std::getenv("LOCALAPPDATA"))
    return fs::u8path(root) / "Project Xenon" / "Launcher Next";
#elif defined(__APPLE__)
  if (const auto *root = std::getenv("HOME"))
    return fs::u8path(root) / "Library/Application Support/Project Xenon/Launcher Next";
#else
  if (const auto *root = std::getenv("XDG_DATA_HOME"))
    return fs::u8path(root) / "xenon/launcher-next";
  if (const auto *root = std::getenv("HOME"))
    return fs::u8path(root) / ".local/share/xenon/launcher-next";
#endif
  throw std::runtime_error("Cannot locate the user application data folder");
}
LauncherService::LauncherService(
    fs::path storage, bool safe_mode, fs::path fixture_host,
    std::function<void(const std::string &, const std::string &)> install_observer,
    std::shared_ptr<IHttpClient> http, std::unique_ptr<IPresenceTransport> presence_transport)
    : storage_(fs::absolute(std::move(storage)).lexically_normal()), storage_lock_(storage_),
      recovery_(storage_, safe_mode), safe_mode_(recovery_.safe_mode()),
      install_observer_(std::move(install_observer)), http_(std::move(http)) {
  storage_ = fs::weakly_canonical(storage_);
  load();
  presence_ = std::make_unique<PresenceService>(XENON_NEXT_DISCORD_APPLICATION_ID,
      presence_transport ? std::move(presence_transport) : discord_rpc_transport());
  if (state_["settings"].value("community/discordRichPresence", false) &&
      (!presence_->available() || state_["settings"].value("community/presenceConsent", "") != presence_consent)) {
    state_["settings"]["community/discordRichPresence"] = false;
    state_["revision"] = state_["revision"].get<std::int64_t>() + 1;
    persist(state_);
  }
  bool media_changed = false;
  for (auto &game : state_["games"])
    if (game.value("mediaModelVersion", 0) != 1 || game.value("activityModelVersion", 0) != 1) {
      normalize_game_media(game);
      normalize_game_activity(game);
      media_changed = true;
    }
  if (media_changed) {
    const auto backup = storage_ / "state.before-media-v1.json";
    if (!fs::exists(backup))
      fs::copy_file(storage_ / "state.json", backup);
    state_["revision"] = state_["revision"].get<std::int64_t>() + 1;
    persist(state_);
  }
  load_online_state();
  if (!safe_mode_ && !fixture_host.empty()) {
    runtime_ = fixture_runtime(std::move(fixture_host), storage_);
    host_services_ = fixture_host_services(storage_);
  } else
    host_services_ = unavailable_host_services();
  recover_install_jobs();
  recover_migrations();
  job_worker_ = std::thread([this] { jobs_worker(); });
}
LauncherService::~LauncherService() {
  {
    std::lock_guard lock(mutex_);
    jobs_stopping_ = true;
    if (cancel_job_)
      cancel_job_->store(true);
  }
  jobs_changed_.notify_all();
  if (job_worker_.joinable())
    job_worker_.join();
  presence_.reset();
  try {
    std::lock_guard lock(mutex_);
    recover_install_jobs();
  } catch (const std::exception &error) {
    std::ofstream(storage_ / "renderer.log", std::ios::app)
        << "Operation shutdown: " << error.what() << '\n';
  }
  try {
    recovery_.clean_exit();
  } catch (const std::exception &error) {
    std::ofstream(storage_ / "renderer.log", std::ios::app)
        << "Recovery shutdown: " << error.what() << '\n';
  }
}
void LauncherService::load() {
  const auto file = storage_ / "state.json";
  if (fs::exists(file)) {
    state_ = read_json(file);
    validate(state_);
    for (auto &module : state_["modules"]) {
      module["schema"] = module_settings_schema(module.at("manifest"));
      module["settings"] =
          module_settings_values(module.at("schema"), module.value("settings", Json::object()));
    }
  } else {
    state_ = {
        {"schemaVersion", 1},
        {"revision", 0},
        {"mode", "native"},
        {"profiles", Json::array({{{"id", "local"},
                                   {"name", "Local player"},
                                   {"description", ""},
                                   {"region", "Auto (Global)"},
                                   {"offline", true},
                                   {"paths", Json::object()}}})},
        {"activeProfileId", "local"},
        {"games", Json::array()},
        {"preferences", {{"theme", "dark"}, {"reducedMotion", false}, {"startPage", "Library"}}},
        {"settings", Json::object()},
        {"modules", Json::array()},
        {"notifications", Json::array()},
        {"history", Json::array()},
        {"jobs", Json::array()}};
    persist(state_);
  }
}
void LauncherService::validate(const Json &state) const {
  if (state.at("schemaVersion") != 1 || !state.at("revision").is_number_integer())
    throw std::runtime_error("Unsupported launcher state version");
  for (const auto *key : {"profiles", "games", "modules", "notifications", "history", "jobs"})
    if (!state.at(key).is_array())
      throw std::runtime_error("Invalid launcher state collection");
  for (const auto &game : state.at("games"))
    validate_game_media(game);
  std::set<std::string> ids;
  bool active = false;
  for (const auto &p : state.at("profiles")) {
    const auto id = p.at("id").get<std::string>();
    if (id.empty() || !ids.insert(id).second || trim(p.at("name")).empty())
      throw std::runtime_error("Invalid profile identity");
    active |= p.at("id") == state.at("activeProfileId");
  }
  if (!active)
    throw std::runtime_error("Active profile is missing");
  ids.clear();
  for (const auto &job : state.at("jobs")) {
    if (!job.is_object() || !job.contains("id") || !job.at("id").is_string() ||
        job.at("id").get<std::string>().empty() ||
        !ids.insert(job.at("id").get<std::string>()).second || !job.at("title").is_string() ||
        !job.at("state").is_string() || !job.at("progress").is_number() ||
        !std::isfinite(job.at("progress").get<double>()) || job.at("progress").get<double>() < 0 ||
        job.at("progress").get<double>() > 100)
      throw std::runtime_error("Invalid saved operation record");
    if (job.contains("_cleanup") && !job.at("_cleanup").is_boolean())
      throw std::runtime_error("Invalid saved operation cleanup flag");
    if (job.value("kind", "") == "module.install") {
      for (const auto *key : {"_source", "_root", "_modified"})
        if (!job.contains(key) || !job.at(key).is_string() ||
            job.at(key).get<std::string>().empty())
          throw std::runtime_error("Invalid saved installation source");
      if (!job.at("_size").is_number_integer() || job.at("_size").get<std::int64_t>() < 0 ||
          job.at("_size").get<std::uint64_t>() > 512ull * 1024 * 1024)
        throw std::runtime_error("Invalid saved installation size");
    }
  }
  const auto &prefs = state.at("preferences");
  if (prefs.at("theme") != "dark" && prefs.at("theme") != "light")
    throw std::runtime_error("Invalid theme");
  if (!prefs.at("reducedMotion").is_boolean() || !prefs.at("startPage").is_string() ||
      !state.at("settings").is_object())
    throw std::runtime_error("Invalid launcher preferences");
}
void LauncherService::persist(const Json &next) {
  validate(next);
  if (next.dump().size() > 16 * 1024 * 1024)
    throw std::runtime_error("Launcher state exceeds the supported size; no data was replaced");
  atomic_json(storage_ / "state.json", next);
  if (state_observer_)
    try {
      state_observer_(next.at("revision").get<std::int64_t>());
    } catch (...) { /* Presentation failure cannot undo an already committed state file. */
    }
}
void LauncherService::observe_changes(std::function<void(std::int64_t)> observer) {
  std::lock_guard lock(mutex_);
  state_observer_ = std::move(observer);
}
void LauncherService::notify(Json &next, const std::string &title, const std::string &message,
                             const std::string &severity, Json action, bool program_title) {
  if (trim(message).empty())
    return;
  if (action.empty()) {
    const auto prefix = lower(title);
    const auto page = prefix.find("module") != std::string::npos    ? "Modules"
                      : prefix.find("profile") != std::string::npos ? "Profiles"
                      : prefix.find("support") != std::string::npos ? "Support"
                                                                    : "Downloads";
    action = {{"type", "navigate"}, {"page", page}};
  }
  const auto time = now();
  if (!next["notifications"].empty()) {
    auto &previous = next["notifications"].back();
    std::int64_t elapsed = 30001;
    try {
      elapsed = std::stoll(time) - std::stoll(previous.value("time", "0"));
    } catch (const std::exception &) {
    }
    if (previous.value("title", "") == title && previous.value("message", "") == message &&
        previous.value("programTitle", false) == program_title &&
        previous.value("severity", "info") == severity &&
        previous.value("action", Json::object()) == action && elapsed >= 0 && elapsed <= 30000) {
      previous["time"] = time;
      previous["read"] = false;
      previous["repeatCount"] = previous.value("repeatCount", 1) + 1;
      return;
    }
  }
  next["notifications"].push_back({{"id", uuid()},
                                   {"title", title},
                                   {"programTitle", program_title},
                                   {"message", message},
                                   {"severity", severity},
                                   {"action", action},
                                   {"read", false},
                                   {"time", time},
                                   {"repeatCount", 1}});
  if (next["notifications"].size() > 120)
    next["notifications"].erase(next["notifications"].begin());
}
Json LauncherService::snapshot() {
  std::lock_guard lock(mutex_);
  poll_runtime();
  return project();
}
Json LauncherService::presentation_settings() const {
  std::lock_guard lock(mutex_);
  return state_.at("settings");
}
Json LauncherService::community_state() const {
  const auto &settings = state_.at("settings");
  presence_->configure(settings.value("community/discordRichPresence", false),
      settings.value("community/presenceConsent", "") == presence_consent, safe_mode_,
      settings.value("community/discordShowGameTitle", true), state_.value("lastPage", "Library"),
      state_.value("session", Json::object()));
  auto result = community_information();
  result["presence"] = presence_->snapshot();
  return result;
}
Json LauncherService::project() const {
  auto result = state_;
  result["community"] = community_state();
  result["settings"].erase("community/presenceConsent");
  result["dlcCatalog"] = launcher_dlc_catalog(state_);
  result["activitySummary"] = game_activity_summary(state_);
  for (auto &module : result["modules"]) {
    module["canUninstall"] = module.value("managed", false);
    if (!module.contains("previousVersions") || !module.at("previousVersions").is_array())
      module["previousVersions"] = Json::array();
    for (auto &previous : module["previousVersions"]) {
      module["canUninstall"] =
          module["canUninstall"].get<bool>() || previous.value("managed", false);
      previous.erase("_installRoot");
      previous.erase("_installOwnerId");
    }
    module.erase("_installRoot");
    module.erase("_installOwnerId");
  }
  for (auto &game : result["games"])
    if (game.contains("managedCopies"))
      for (auto &copy : game["managedCopies"])
        copy = {{"root", copy.at("root")},
                {"entryId", copy.at("entryId")},
                {"locationId", copy.at("_ownerId")},
                {"type", copy.at("_type")},
                {"previous", copy.value("previous", false)},
                {"originalPath", copy.at("originalPath")},
                {"originalRetained", copy.value("originalRetained", true)}};
  for (auto &job : result["jobs"]) {
    if (job.contains("_gameId"))
      job["gameId"] = job.at("_gameId");
    job["cleaningUp"] = job.value("_cleanup", false);
    for (auto field = job.begin(); field != job.end();)
      if (!field.key().empty() && field.key().front() == '_')
        field = job.erase(field);
      else
        ++field;
    const auto status = job.value("state", "");
    const auto kind = job.value("kind", "");
    const bool supported = supported_job_kind(kind);
    job["canCancel"] = supported && (status == "queued" || status == "running");
    job["canRetry"] =
        supported && (status == "failed" || status == "cancelled" || status == "interrupted");
  }
  result["mode"] = "native";
  result["capabilities"] = {{"runtime", runtime_ && runtime_->available()},
                            {"importContent", true},
                            {"modules", true},
                            {"captures", true},
                            {"online", http_ != nullptr},
                            {"networkFixture", http_ && http_->fixture()}};
  project_online(result);
  if (!runtime_)
    result["session"] = {{"state", "unavailable"}, {"fixture", false}};
  else if (result.contains("session") &&
           result["session"].value("sessionId", "") == dismissed_session_)
    result["session"] = {{"state", "idle"}, {"fixture", true}};
  for (auto &profile : result["profiles"])
    profile["effectiveRuntimeSettings"] = profile_runtime_values(
        Json::object(), effective_profile_settings(profile, state_["settings"]), true);
  const auto &active = item(result["profiles"], state_.at("activeProfileId"));
  const auto startup = active.value("startupPage", "Launcher default");
  if (state_["settings"].value("general/restoreLastPage", false) && state_.contains("lastPage"))
    result["preferences"]["startPage"] = state_.at("lastPage");
  else if (startup != "Launcher default" && !startup.empty())
    result["preferences"]["startPage"] = startup;
  if (safe_mode_)
    result["preferences"]["startPage"] = "Settings";
  result["runtimeServices"] = host_services_->snapshot();
  result["storagePath"] = path_string(storage_);
  result["application"] = launcher_build_info();
  result["locations"] = launcher_locations(storage_, state_["settings"]);
  const auto cache = cache_location(storage_, state_);
  const auto root = cache_root(storage_, cache);
  const bool cache_pending =
      std::any_of(state_["jobs"].begin(), state_["jobs"].end(), [](const auto &job) {
        const auto status = job.value("state", "");
        return cache_relocation_job(job) &&
               (status == "queued" || status == "running" || status == "cancelling" ||
                job.value("_cleanup", false));
      });
  result["cache"] = {{"configuredPath", cache.value("configuredPath", "")},
                     {"effectivePath", path_string(root)},
                     {"pending", cache_pending},
                     {"previousLocations", Json::array()}};
  for (const auto &old : state_.value("cacheHistory", Json::array()))
    result["cache"]["previousLocations"].push_back(old.at("root"));
  result.erase("cacheHistory");
  result["locations"]["destinations"].push_back(
      {{"id", "cache"},
       {"key", "paths/cache"},
       {"label", "Cache"},
       {"defaultPath", path_string(storage_)},
       {"configuredPath", cache.value("configuredPath", "")},
       {"effectivePath", path_string(root)},
       {"invalid", false},
       {"description",
        "Changing this location copies and verifies managed artwork and downloads in the "
        "background. Previous copies are retained. Browser cache stays with launcher data."}});
  for (auto &location : result["locations"]["fixed"]) {
    if (location.at("id") == "artwork") {
      location["path"] = path_string(root / "catalog-art");
      location["description"] = "Managed artwork follows the Cache destination.";
      if (state_.contains("cache"))
        location["legacyConfiguredPath"] = "";
    }
    if (location.at("id") == "downloads") {
      location["path"] = path_string(root / "downloads");
      location["description"] = "Verified packages follow the Cache destination; installations use "
                                "the Modules destination.";
    }
  }
  result["recovery"] = recovery_.snapshot();
  result["recovery"]["previousUncleanExit"] = result["recovery"].value("pendingIncident", false);
  captures_.folder(launcher_destination(storage_, state_["settings"], "paths/screenshots"));
  const auto captures = captures_.snapshot();
  result["captures"] = captures.at("entries");
  result["captureFolderPath"] =
      path_string(launcher_destination(storage_, state_["settings"], "paths/screenshots"));
  result["captureScan"] = captures;
  result["captureScan"].erase("entries");
  return result;
}
void LauncherService::poll_runtime() {
  if (!runtime_)
    return;
  const auto report = runtime_->status();
  const auto previous = state_.value("session", Json::object());
  auto next = state_;
  next["session"] = report;
  const bool activity_changed = apply_session_activity(next, report);
  const bool changed = activity_changed ||
                       previous.value("state", "") != report.value("state", "") ||
                       previous.value("sessionId", "") != report.value("sessionId", "");
  if (changed) {
    if (previous.value("state", "") != "failed" && report.value("state", "") == "failed")
      notify(next, report.value("fixture", true) ? "Test session failed" : "Session failed",
             report.value("lastError", "Unknown failure"));
    next["revision"] = state_["revision"].get<std::int64_t>() + 1;
    persist(next);
    state_ = std::move(next);
  } else
    state_["session"] = report;
}
Json LauncherService::launch_configuration(const Json &state, const std::string &game_id) const {
  const auto &games = state.at("games"), &profiles = state.at("profiles");
  const auto &game = item(games, game_id);
  const auto &profile = item(profiles, state.at("activeProfileId"));
  const auto source = game.value("contentPath", "");
  if (!fs::exists(fs::u8path(source)))
    throw std::runtime_error("Game source is missing");
  const auto settings = effective_profile_settings(profile, state.at("settings"));
  Json config = {{"gameId", game_id},
                 {"title", game.at("title")},
                 {"contentPath", source},
                 {"primaryMediaId",
                  [&] {
                    for (const auto &m : game.at("media"))
                      if (m.value("isPrimary", false))
                        return m.at("id").get<std::string>();
                    return std::string{};
                  }()},
                 {"media", game.at("media")},
                 {"moduleId", game.value("moduleId", "")},
                 {"profileId", profile.at("id")},
                 {"profileName", profile.at("name")},
                 {"region", profile.value("region", "Auto (Global)")},
                 {"offline", profile.value("offline", true)},
                 {"isolatedSettings", profile.value("isolatedSettings", false)},
                 {"renderer", settings.value("runtime/graphicsBackend", "Automatic")},
                 {"shaderCache", settings.value("graphics/shaderCache", true)},
                 {"shaderCacheMode", settings.value("graphics/shaderCacheMode", "Persistent")},
                 {"inputBackend", settings.value("input/backend", "Automatic")},
                 {"inputPreferredDevice", settings.value("input/preferredDevice", "Automatic")},
                 {"inputDeadzone", settings.value("input/deadzone", .1)},
                 {"inputRumble", settings.value("input/rumble", true)},
                 {"inputBackground", settings.value("input/backgroundInput", false)},
                 {"audioMasterVolume", settings.value("audio/masterVolume", 1.0)},
                 {"audioMuteUnfocused", settings.value("audio/muteUnfocused", false)},
                 {"audioLatencyProfile", settings.value("audio/latencyProfile", "Automatic")},
                 {"logVerbose", settings.value("developer/verboseLogging", false)},
                 {"moduleSettings", Json::object()},
                 {"runtimeApiRequirements", Json::object()},
                 {"fixture", true}};
  for (const auto &pair : {std::pair{"gamePath", "gameRoot"}, std::pair{"savePath", "savePath"},
                           std::pair{"screenshotPath", "screenshotsPath"}}) {
    const auto override_path = profile.value(pair.first, "");
    validate_launcher_path(override_path);
    const auto key = std::string(pair.first) == "gamePath"   ? "paths/games"
                     : std::string(pair.first) == "savePath" ? "paths/saves"
                                                             : "paths/screenshots";
    config[pair.second] = !override_path.empty()
                              ? override_path
                              : path_string(launcher_destination(storage_, settings, key));
  }
  const auto module_id = game.value("moduleId", "");
  if (!module_id.empty()) {
    const auto &modules = state.at("modules");
    const auto &module = item(modules, module_id);
    if (!module.value("enabled", true))
      throw std::runtime_error("The selected module is disabled");
    config["moduleName"] = module.at("name");
    config["modulePath"] = module.at("path");
    config["moduleVersion"] = module.at("version");
    config["moduleSettings"] = module_settings_values(module.at("schema"), module.at("settings"));
    require_module_idle(state, module_id);
    const auto definition = manifest(fs::u8path(module.at("path").get<std::string>()));
    if (module_identity(definition) != module_id)
      throw std::runtime_error("Module identity changed; rescan or register the correct module");
    const auto compatibility =
        module_compatibility(definition, fs::u8path(module.at("path").get<std::string>()));
    if (!compatibility.at("issues").empty())
      throw std::runtime_error(compatibility.at("issues")[0].get<std::string>());
    config["runtimeApiRequirements"] = module_runtime_apis(definition);
    config["nativeExtensionPath"] = compatibility.at("nativeExtensionPath");
    config["moduleCompatibility"] = compatibility;
  }
  config["inputRouting"] = host_services_->snapshot().at("input").at("users");
  config["dlc"] = game.value("dlc", Json::array());
  return config;
}
std::optional<fs::path> LauncherService::asset_path(const std::string &category,
                                                    const std::string &id) const {
  std::lock_guard lock(mutex_);
  if (id.empty() || id.find('/') != std::string::npos || id.find('\\') != std::string::npos ||
      id == "." || id == "..")
    return {};
  fs::path root;
  fs::path file;
  bool nested = false;
  if (category == "catalog-tile" || category == "catalog-hero") {
    const auto &entry = item(state_.at("catalog").at("entries"), id);
    const auto key = category == "catalog-tile" ? "_tileFile" : "_heroFile";
    const auto filename = entry.at("launcher").value(key, "");
    const auto relative = fs::u8path(filename);
    if (filename.empty() || relative.filename() != relative)
      return {};
    const auto location = cache_location(storage_, state_);
    check_cache_location(storage_, location);
    root = fs::weakly_canonical(cache_root(storage_, location) / "catalog-art");
    file = root / relative;
  } else if (category == "captures") {
    root = fs::weakly_canonical(
        launcher_destination(storage_, state_["settings"], "paths/screenshots"));
    file = root / fs::u8path(id);
  } else if (category == "avatars") {
    const auto &profile = item(state_.at("profiles"), id);
    const auto key = profile.value("avatarKey", "");
    if (key.empty() || key.find('/') != std::string::npos || key.find('\\') != std::string::npos)
      return {};
    root = fs::weakly_canonical(storage_ / "avatars");
    file = root / fs::u8path(key);
  } else if (category == "game-tile" || category == "game-hero") {
    const auto &game = item(state_.at("games"), id);
    const auto &module = item(state_.at("modules"), game.value("moduleId", ""));
    const auto metadata = module.at("manifest").value("launcher", Json::object());
    const auto key = category == "game-tile" ? "tileArt" : "heroArt";
    if (!metadata.is_object() || !metadata.contains(key) || !metadata[key].is_string())
      return {};
    root = fs::weakly_canonical(fs::u8path(module.at("path").get<std::string>()));
    file = root / fs::u8path(metadata[key].get<std::string>());
    nested = true;
  } else
    return {};
  const auto ext = lower(path_string(file.extension()));
  if (!capture_extension(file))
    return {};
  std::error_code error;
  const auto canonical = fs::weakly_canonical(file, error);
  const auto relative = canonical.lexically_relative(root);
  if (fs::is_symlink(file) || error || relative.empty() || *relative.begin() == ".." ||
      (!nested && canonical.parent_path() != root) || !fs::is_regular_file(file, error) ||
      fs::file_size(file, error) > 20 * 1024 * 1024)
    return {};
  return file;
}
Json LauncherService::request(const Json &request) {
  std::string id;
  try {
    id = request.value("id", "");
    if (request.at("version") != 1 || id.empty() || !request.at("command").is_string() ||
        !request.at("payload").is_object())
      throw std::runtime_error("Invalid launcher request envelope");
    Json result;
    if (request.at("command") == "application.snapshot")
      result = snapshot();
    else {
      std::lock_guard lock(mutex_);
      const auto command = request.at("command").get<std::string>();
      if (command == "community.state" || command == "community.refresh") {
        if (command == "community.refresh") presence_->refresh();
        result = community_state();
      } else if (command == "community.info") result = community_information();
      else result = execute(command, request.at("payload"));
    }
    Json response = {{"version", 1}, {"id", id}, {"ok", true}};
    response["data"] = std::move(result);
    return response;
  } catch (const std::exception &e) {
    return {{"version", 1},
            {"id", id},
            {"ok", false},
            {"error", {{"code", "COMMAND_FAILED"}, {"message", e.what()}}}};
  }
}
void LauncherService::register_installed_module(Json &next, const Json &definition,
                                                const fs::path &module_path) {
  (void)normalize_dlc_definitions(definition);
  const auto id = module_identity(definition);
  require_module_idle(next, id);
  const auto schema = module_settings_schema(definition);
  Json replacement = {{"id", id},
                      {"name", definition.value("name", id)},
                      {"version", definition.value("version", "Unknown")},
                      {"path", path_string(module_path)},
                      {"enabled", true},
                      {"schema", schema},
                      {"settings", module_settings_values(schema, Json::object())},
                      {"manifest", definition},
                      {"compatibility", module_compatibility(definition, module_path)},
                      {"managed", true},
                      {"previousVersions", Json::array()}};
  auto previous = std::find_if(next["modules"].begin(), next["modules"].end(),
                               [&](const auto &module) { return module.at("id") == id; });
  const auto previous_version =
      previous == next["modules"].end() ? "" : previous->value("version", "");
  if (previous != next["modules"].end()) {
    auto history = previous->value("previousVersions", Json::array());
    auto backup = *previous;
    backup.erase("previousVersions");
    history.push_back(backup);
    if (history.size() > 5)
      history.erase(history.begin());
    replacement["previousVersions"] = history;
    replacement["settings"] =
        module_settings_values(schema, previous->value("settings", Json::object()));
    replacement["enabled"] = previous->value("enabled", true);
    *previous = replacement;
  } else
    next["modules"].push_back(replacement);
  notify(next, "Module package installed",
         definition.value("name", id) + " " + definition.value("version", "Unknown"));
  next["updateHistory"].push_back({{"id", uuid()},
                                   {"moduleId", id},
                                   {"action", "install"},
                                   {"outcome", "success"},
                                   {"fromVersion", previous_version},
                                   {"toVersion", definition.value("version", "")},
                                   {"message", "Module package installed"},
                                   {"time", now()}});
  while (next["updateHistory"].size() > 200)
    next["updateHistory"].erase(next["updateHistory"].begin());
  for (auto &game : next["games"])
    if (game.value("moduleId", "") == id) {
      game["module"] = replacement.at("name");
      module_metadata(game, replacement, next["revision"].get<std::int64_t>() + 1, false,
                      presentation_catalog_entry(next, id));
    }
}
void LauncherService::refresh_module_presentations(Json &next) {
  std::map<std::string, const Json *> modules, catalog;
  for (const auto &module : next["modules"])
    modules[module.at("id").get<std::string>()] = &module;
  for (const auto &entry : next["catalog"]["entries"])
    catalog[entry.at("id").get<std::string>()] = &entry;
  for (auto &game : next["games"]) {
    const auto id = game.value("moduleId", "");
    const auto found = modules.find(id), presentation = catalog.find(id);
    if (found != modules.end()) {
      game["module"] = found->second->at("name");
      module_metadata(game, *found->second, next["revision"].get<std::int64_t>() + 1, false,
                      presentation == catalog.end() ? nullptr : presentation->second);
    }
  }
}
Json LauncherService::execute(const std::string &command, const Json &payload) {
  auto next = state_;
  std::string cancel_requested;
  const auto action =
      command == "launcher.action" ? payload.at("action").get<std::string>() : command;
  const auto data = command == "launcher.action" ? payload.value("data", Json::object()) : payload;
  if (update_preparing_ && action != "application.ready")
    throw std::runtime_error("Wait for launcher replacement preparation to finish");
  for (const auto &job : next["jobs"])
    if (job.value("_command", "") == "modules.uninstall" &&
        job.value("state", "") == "committing" &&
        (action.starts_with("library.") || action.starts_with("modules.") ||
         action.starts_with("profiles.") || action.starts_with("settings.") ||
         action == "session.launch"))
      throw std::runtime_error("Wait for owned module file removal to finish");
  if (action.starts_with("modules.") && data.contains("id") && action != "modules.checkUpdate" &&
      action != "modules.downloadUpdate")
    require_module_idle(next, data.at("id").get<std::string>());
  if (action == "profiles.create" || action == "profiles.update" ||
      action == "profiles.duplicate") {
    const auto id = data.value("id", "");
    Json profile = action == "profiles.create"
                       ? Json{{"id", uuid()},
                              {"description", ""},
                              {"region", "Auto (Global)"},
                              {"offline", next["settings"].value("runtime/offline", true)},
                              {"paths", Json::object()}}
                       : item(next["profiles"], id);
    auto name = trim_profile_name(data.value("name", profile.value("name", "")));
    if (action == "profiles.duplicate") {
      name = unique_profile_name(next["profiles"], name_prefix(name, 43) + " copy");
      profile["id"] = uuid();
    }
    validate_name(next["profiles"], name, action == "profiles.update" ? id : "",
                  action == "profiles.update" && name == profile.value("name", ""));
    profile["name"] = name;
    if (data.contains("description") && data.at("description").is_string()) {
      const auto value = data.at("description").get<std::string>();
      if (value != profile.value("description", "") && name_prefix(value, 180) != value)
        throw std::runtime_error("Use a profile description of at most 180 characters");
    }
    for (const auto *key :
         {"description", "region", "startupPage", "gamePath", "savePath", "screenshotPath"})
      if (data.contains(key)) {
        if (!data[key].is_string())
          throw std::runtime_error("Invalid profile field");
        if (std::string(key) == "gamePath" || std::string(key) == "savePath" ||
            std::string(key) == "screenshotPath")
          validate_launcher_path(data[key]);
        profile[key] = data[key];
      }
    for (const auto *key : {"avatarFocalX", "avatarFocalY", "avatarZoom"})
      if (data.contains(key)) {
        const auto value = data[key].get<double>();
        if (!std::isfinite(value) || value < (std::string(key) == "avatarZoom" ? 1.0 : 0.0) ||
            value > (std::string(key) == "avatarZoom" ? 3.0 : 1.0))
          throw std::runtime_error("Invalid avatar crop");
        profile[key] = value;
      }
    for (const auto *key : {"offline", "isolatedSettings"})
      if (data.contains(key)) {
        if (!data[key].is_boolean())
          throw std::runtime_error("Invalid profile flag");
        profile[key] = data[key];
      }
    if (data.contains("runtimeOverrides"))
      profile["runtimeOverrides"] = data.at("runtimeOverrides");
    normalize_profile_preferences(profile, next["settings"]);
    if (action == "profiles.update")
      item(next["profiles"], id) = profile;
    else
      next["profiles"].push_back(profile);
    notify(next, action == "profiles.update" ? "Profile saved" : "Profile created", name, "success",
           {{"type", "navigate"}, {"page", "Profiles"}, {"targetId", profile.at("id")}});
  } else if (action == "profiles.activate") {
    (void)item(next["profiles"], data.at("id"));
    next["activeProfileId"] = data.at("id");
  } else if (action == "profiles.remove") {
    if (data.at("id") == next["activeProfileId"])
      throw std::runtime_error("Activate another profile before removing this one");
    remove_item(next["profiles"], data.at("id"));
  } else if (action == "profiles.avatarImport") {
    auto &profile = item(next["profiles"], data.at("id"));
    const auto source = fs::u8path(data.at("path").get<std::string>());
    const auto ext = lower(path_string(source.extension()));
    if ((ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".webp") ||
        !fs::is_regular_file(source) || fs::file_size(source) > 20 * 1024 * 1024)
      throw std::runtime_error("Choose a PNG, JPEG or WebP image under 20 MiB");
    const auto key = uuid() + ext;
    fs::create_directories(storage_ / "avatars");
    fs::copy_file(source, storage_ / "avatars" / fs::u8path(key));
    profile["avatarKey"] = key;
    profile["avatarFocalX"] = .5;
    profile["avatarFocalY"] = .5;
    profile["avatarZoom"] = 1.0;
  } else if (action == "profiles.avatarRemove") {
    auto &profile = item(next["profiles"], data.at("id"));
    profile.erase("avatarKey");
  } else if (action == "profiles.export") {
    auto exported = item(next["profiles"], data.at("id"));
    exported["profileName"] = exported.at("name");
    for (const auto *key : {"id", "name", "avatarPath", "avatarKey", "avatarFocalX", "avatarFocalY",
                            "avatarZoom", "paths", "gamePath", "savePath", "screenshotPath"})
      exported.erase(key);
    atomic_json(fs::u8path(data.at("path").get<std::string>()),
                {{"schema", "xenon.launcher.profile"}, {"version", 1}, {"profile", exported}});
  } else if (action == "profiles.import") {
    auto document = read_json(fs::u8path(data.at("path").get<std::string>()));
    if (document.value("schema", "") != "xenon.launcher.profile" ||
        document.value("version", 0) != 1 || !document.at("profile").is_object())
      throw std::runtime_error("Unsupported Xenon profile format");
    const auto source = document.at("profile");
    const auto base =
        trim_profile_name(source.value("profileName", source.value("name", "Imported profile")));
    const auto name = unique_profile_name(next["profiles"], base);
    validate_name(next["profiles"], name);
    Json profile = {{"id", uuid()},
                    {"name", name},
                    {"description", source.value("description", "")},
                    {"region", source.value("region", "Auto (Global)")},
                    {"offline", source.value("offline", true)},
                    {"isolatedSettings", source.value("isolatedSettings", false)},
                    {"runtimeOverrides",
                     source.value("isolatedSettings", false)
                         ? profile_runtime_values(source.value("runtimeOverrides", Json::object()),
                                                  next["settings"], true)
                         : Json::object()},
                    {"startupPage", source.value("startupPage", "Launcher default")},
                    {"paths", Json::object()}};
    normalize_profile_preferences(profile, next["settings"]);
    next["profiles"].push_back(profile);
  } else if (action == "settings.update") {
    for (const auto &[key, value] : data.items()) {
      if (key != "theme" && key != "reducedMotion" && key != "startPage")
        throw std::runtime_error("Unknown preference");
      next["preferences"][key] = value;
    }
  } else if (action == "settings.set") {
    const auto key = data.at("key").get<std::string>();
    const auto &value = data.at("value");
    validate_setting(key, value, presence_->available() && !safe_mode_);
    if (key.starts_with("network/") &&
        !host_services_->snapshot().at("capabilities").value("networkConfiguration", false))
      throw std::runtime_error(
          "Runtime network configuration requires a connected configuration provider");
    if (key == "paths/cache") {
      enqueue_service(next, "cache.relocate", {{"path", value}});
    } else {
      if (key.starts_with("paths/")) {
        validate_launcher_path(value);
        if (!launcher_destination_key(key) && value != "")
          throw std::runtime_error("This storage location cannot be relocated from Settings yet. "
                                   "Existing legacy values are preserved for review.");
      }
      next["settings"][key] = value;
      if (key == "community/discordRichPresence") {
        if (value == true) next["settings"]["community/presenceConsent"] = presence_consent;
        else next["settings"].erase("community/presenceConsent");
      }
      if (key == "general/startupPage")
        next["preferences"]["startPage"] = value;
      if (key == "accessibility/reduceMotion")
        next["preferences"]["reducedMotion"] = value;
    }
  } else if (action == "settings.resetKey") {
    const auto key = data.at("key").get<std::string>();
    const auto &definitions = settings_definitions();
    const auto definition = std::find_if(definitions.begin(), definitions.end(),
                                         [&](const auto &value) { return value.at("key") == key; });
    if (definition == definitions.end())
      validate_setting(key, "");
    if (key == "paths/cache")
      enqueue_service(next, "cache.relocate", {{"path", ""}});
    else
      next["settings"].erase(key);
    if (key == "general/startupPage")
      next["preferences"]["startPage"] = definition->at("default");
    if (key == "accessibility/reduceMotion")
      next["preferences"]["reducedMotion"] = definition->at("default");
  } else if (action == "settings.reset") {
    const auto category = data.value("category", "");
    if (category.empty() || category == "paths")
      enqueue_service(next, "cache.relocate", {{"path", ""}});
    std::set<std::string> keys;
    for (const auto &definition : settings_definitions())
      if (definition.value("category", "") == category)
        keys.insert(definition.at("key"));
    for (auto it = next["settings"].begin(); it != next["settings"].end();)
      if (category.empty() || keys.contains(it.key()) || it.key().rfind(category + "/", 0) == 0)
        it = next["settings"].erase(it);
      else
        ++it;
    if (category.empty() || category == "general")
      next["preferences"]["startPage"] = "Library";
    if (category.empty() || category == "appearance")
      next["preferences"]["theme"] = "dark";
    if (category.empty() || category == "accessibility")
      next["preferences"]["reducedMotion"] = false;
  } else if (action.starts_with("library.media.")) {
    execute_media(next, action, data);
  } else if (library_job_kind(action)) {
    enqueue_library(next, action, data);
  } else if (action == "library.import") {
    if (data.value("mode", "keep") == "move") {
      for (const auto &source : data.at("paths")) {
        Json options = {{"path", source}, {"mode", "move"}, {"type", "game"}};
        const auto path = path_string(fs::canonical(fs::u8path(source.get<std::string>())));
        for (const auto &game : next["games"])
          for (const auto &medium : game.at("media"))
            if (same_media_source(medium.at("sourcePath"), path)) {
              options["id"] = game.at("id");
              options["mediaId"] = medium.at("id");
            }
        enqueue_library(next, "library.transfer", options);
      }
    } else {
      for (const auto &source : data.at("paths")) {
        const auto path = fs::weakly_canonical(fs::u8path(source.get<std::string>()));
        if (!fs::exists(path))
          throw std::runtime_error("Game source does not exist");
        const auto text = path_string(path);
        if (std::any_of(next["games"].begin(), next["games"].end(), [&](const auto &g) {
              return std::any_of(g.at("media").begin(), g.at("media").end(), [&](const auto &m) {
                return same_media_source(m.at("sourcePath"), text);
              });
            }))
          continue;
        if (!fs::is_directory(path)) {
          const auto ext = lower(path_string(path.extension()));
          if (ext != ".xex" && ext != ".iso" && ext != ".xgd" && ext != ".dvd")
            throw std::runtime_error("Choose a game folder, XEX or disc image");
        }
        next["games"].push_back(
            {{"id", uuid()},
             {"title", path_string(path.stem())},
             {"module", "No compatible module selected"},
             {"moduleId", ""},
             {"description", "Local source registered. Content identification and compatibility "
                             "require the Xenon content adapter."},
             {"contentPath", text},
             {"favorite", false},
             {"lastPlayed", nullptr},
             {"addedAt", now()},
             {"dlc", Json::array()},
             {"verified", false}});
        normalize_game_media(next["games"].back());
        normalize_game_activity(next["games"].back());
      }
      notify(next, "Library updated",
             "Local sources registered; compatibility has not been verified.");
    }
  } else if (action == "library.favorite") {
    item(next["games"], data.at("id"))["favorite"] = data.at("favorite").get<bool>();
  } else if (action == "library.remove") {
    require_library_idle(next, data.at("id"));
    if (!item(next["games"], data.at("id")).value("managedCopies", Json::array()).empty())
      throw std::runtime_error("Delete managed files before removing this library entry");
    remove_item(next["games"], data.at("id"));
  } else if (action == "library.update") {
    auto &game = item(next["games"], data.at("id"));
    if (data.contains("title")) {
      const auto title = trim(data.at("title"));
      if (title.empty())
        throw std::runtime_error("Enter a game title");
      game["title"] = title;
    }
    if (data.contains("moduleId")) {
      require_library_idle(next, data.at("id"));
      const auto id = data.at("moduleId").get<std::string>();
      if (id.empty()) {
        game["moduleId"] = "";
        game["module"] = "No compatible module selected";
        game.erase("tileArt");
        game.erase("heroArt");
      } else {
        require_module_idle(next, id);
        auto &mod = item(next["modules"], id);
        game["moduleId"] = mod.at("id");
        game["module"] = mod.at("name");
        module_metadata(game, mod, next["revision"].get<std::int64_t>() + 1, false,
                        presentation_catalog_entry(next, id));
      }
    }
  } else if (action == "library.refreshMetadata") {
    auto &game = item(next["games"], data.at("id"));
    const auto &module = item(next["modules"], game.value("moduleId", ""));
    const auto entry = presentation_catalog_entry(next, game.value("moduleId", ""));
    module_metadata(game, module, next["revision"].get<std::int64_t>() + 1, true, entry);
    if (entry && http_) {
      auto pending = std::find_if(next["jobs"].begin(), next["jobs"].end(), [](const auto &job) {
        const auto status = job.value("state", "");
        return job.value("kind", "") == "catalog.refresh" &&
               (status == "queued" || status == "running" || status == "committing");
      });
      if (pending == next["jobs"].end()) {
        enqueue_network(next, "catalog.refresh");
        pending = next["jobs"].end() - 1;
      }
      if (!pending->contains("_metadataRefresh"))
        (*pending)["_metadataRefresh"] = Json::object();
      (*pending)["_metadataRefresh"][game.at("id").get<std::string>()] = {
          {"moduleId", game.value("moduleId", "")}, {"expectedTitle", game.at("title")}};
      notify(next, "Metadata refresh queued",
             "Cached metadata is applied. The official catalogue and artwork will refresh in "
             "Downloads.");
    } else
      notify(next, "Cached metadata applied",
             entry ? "Online services are disabled; cached catalogue metadata was used."
                   : "The installed module manifest was used; this module is not in the saved "
                     "catalogue.");
  } else if (action == "library.verify") {
    auto &game = item(next["games"], data.at("id"));
    if (!fs::exists(fs::u8path(game.at("contentPath").get<std::string>())))
      throw std::runtime_error("The registered source is missing");
    notify(next, "Source exists",
           "The source path exists. Game integrity and compatibility are not "
           "validated without the content adapter.");
  } else if (action == "dlc.import") {
    if (data.contains("catalogId")) {
      const auto &paths = data.at("paths");
      if (!paths.is_array() || paths.empty() || paths.size() > 128)
        throw std::runtime_error("Choose between one and 128 add-on sources");
      enqueue_library(next, "library.transfer",
                      {{"id", data.at("id")},
                       {"path", paths.front()},
                       {"paths", paths},
                       {"catalogId", data.at("catalogId")},
                       {"type", "dlc"},
                       {"mode", "copy"}});
    } else {
      for (const auto &source : data.at("paths"))
        enqueue_library(
            next, "library.transfer",
            {{"id", data.at("id")}, {"path", source}, {"type", "dlc"}, {"mode", "copy"}});
    }
  } else if (action == "dlc.verify") {
    auto &dlc = item(item(next["games"], data.at("id"))["dlc"], data.at("dlcId"));
    dlc["receiptVerified"] = false;
    if (dlc.value("managed", false))
      enqueue_library(next, "library.verifyManaged",
                      {{"id", data.at("id")}, {"dlcId", data.at("dlcId")}});
    else {
      if (!fs::exists(fs::u8path(dlc.at("path").get<std::string>())))
        throw std::runtime_error("The add-on source is missing");
      dlc["lastCheckedAt"] = now();
      notify(next, "Add-on source exists",
             "Source path exists. Content integrity requires the Xenon adapter.");
    }
  } else if (action == "dlc.remove") {
    require_library_idle(next, data.at("id"));
    auto &game = item(next["games"], data.at("id"));
    if (item(game["dlc"], data.at("dlcId")).value("managed", false))
      enqueue_library(next, "library.deleteManaged",
                      {{"id", data.at("id")}, {"dlcId", data.at("dlcId")}});
    else
      remove_item(game["dlc"], data.at("dlcId"));
  } else if (action == "modules.import") {
    const auto folder = fs::weakly_canonical(fs::u8path(data.at("path").get<std::string>()));
    const auto definition = manifest(folder);
    (void)normalize_dlc_definitions(definition);
    const auto id = module_identity(definition);
    if (std::any_of(next["modules"].begin(), next["modules"].end(),
                    [&](const auto &m) { return m.at("id") == id; }))
      throw std::runtime_error("This module is already registered");
    next["modules"].push_back(
        {{"id", id},
         {"name", definition.value("name", id)},
         {"version", definition.value("version", "Unknown")},
         {"path", path_string(folder)},
         {"enabled", true},
         {"schema", module_settings_schema(definition)},
         {"settings", module_settings_values(module_settings_schema(definition), Json::object())},
         {"manifest", definition}});
    next["modules"].back()["compatibility"] = module_compatibility(definition, folder);
    notify(next, "Module registered", definition.value("name", id));
  } else if (action == "modules.installPackage") {
    enqueue_install(next, fs::u8path(data.at("path").get<std::string>()));
  } else if (action == "catalog.refresh") {
    enqueue_network(next, "catalog.refresh");
  } else if (action == "modules.checkUpdate" || action == "modules.downloadUpdate") {
    enqueue_network(next, action == "modules.checkUpdate" ? "module.check" : "module.download",
                    data.at("id"));
  } else if (action == "modules.checkAllUpdates") {
    enqueue_network(next, "module.checkAll");
  } else if (action == "updates.check" || action == "updates.download") {
    enqueue_network(next, action == "updates.check" ? "launcher.check" : "launcher.download",
                    "launcher");
  } else if (action == "modules.installDownloaded") {
    const auto id = data.at("id").get<std::string>();
    const auto &value = next.at("moduleUpdates").at(id);
    const auto &release = value.at("release");
    if (value.value("downloadVersion", "") != release.value("version", "") ||
        value.value("downloadDigest", "") != release.at("asset").value("sha256", ""))
      throw std::runtime_error("Download and verify the selected release before installing");
    enqueue_install(next, fs::u8path(value.at("_packagePath").get<std::string>()));
    auto &job = next["jobs"].back();
    job["_sha256"] = value.at("downloadDigest");
    job["_expectedModuleId"] = id;
    job["moduleId"] = id;
    job["_expectedVersion"] = release.at("version");
  } else if (action == "updates.clearHistory") {
    const auto id = data.value("id", "");
    auto &rows = next["updateHistory"];
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [&](const auto &value) {
                                return id.empty() || value.value("moduleId", "") == id;
                              }),
               rows.end());
  } else if (action == "updates.initialize") {
    if (online_initialized_)
      return project();
    online_initialized_ = true;
    if (http_ && !safe_mode_ && next["settings"].value("updates/automaticChecks", true)) {
      const auto interval = next["settings"].value("updates/checkInterval", "Daily");
      std::int64_t last = 0;
      try {
        last = std::stoll(next.value("lastAutomaticCheck", "0"));
      } catch (...) {
      }
      const auto current = std::stoll(now());
      const auto delay = interval == "Weekly" ? 7ll * 24 * 3600 * 1000 : 24ll * 3600 * 1000;
      if (interval != "Manual" && (interval == "At startup" || current - last >= delay)) {
        if (next["settings"].value("updates/modules", true)) {
          enqueue_network(next, "catalog.refresh");
          next["jobs"].back()["_checkInstalled"] = true;
        }
        enqueue_network(next, "launcher.check", "launcher");
        next["lastAutomaticCheck"] = now();
      }
    }
  } else if (migration_job_kind(action)) {
    enqueue_migration(next, action, data);
  } else if (host_service_command(action) || action == "cache.relocate" ||
             action == "modules.rescan" || action == "modules.uninstall") {
    enqueue_service(next, action, data);
  } else if (action == "jobs.cancel") {
    auto &job = item(next["jobs"], data.at("id"));
    if (!supported_job_kind(job.value("kind", "")))
      throw std::runtime_error("This operation cannot be cancelled");
    const auto status = job.value("state", "");
    if (status == "queued") {
      job["state"] = "cancelled";
      job["phase"] = "Cancelled";
      job["endedAt"] = now();
      apply_online_error(next, job, "Online operation cancelled");
    } else if (status == "running") {
      job["state"] = "cancelling";
      job["phase"] = "Cancelling";
      cancel_requested = data.at("id");
    } else
      throw std::runtime_error("This operation can no longer be cancelled");
  } else if (action == "jobs.retry") {
    const auto job = item(next["jobs"], data.at("id"));
    const auto status = job.value("state", "");
    if (!supported_job_kind(job.value("kind", "")) ||
        (status != "failed" && status != "cancelled" && status != "interrupted"))
      throw std::runtime_error("This operation cannot be retried");
    if (job.value("kind", "") == "module.install") {
      enqueue_install(next, fs::u8path(job.at("_source").get<std::string>()));
      for (const auto *key : {"_sha256", "_expectedModuleId", "_expectedVersion"})
        if (job.contains(key))
          next["jobs"].back()[key] = job.at(key);
    } else if (migration_job_kind(job.value("kind", "")))
      enqueue_migration(next, job.at("kind"), job.at("_options"));
    else if (library_job_kind(job.value("kind", ""))) {
      if (job.value("_committed", false)) {
        require_library_idle(next, job.at("_gameId"));
        unsigned active = 0;
        for (const auto &value : next["jobs"])
          if (value.value("state", "") == "queued" || value.value("state", "") == "running" ||
              value.value("state", "") == "cancelling" || value.value("state", "") == "committing")
            ++active;
        if (active >= 32)
          throw std::runtime_error("The operation queue is full");
        auto retry = job;
        retry["id"] = make_id();
        retry["state"] = "queued";
        retry["phase"] = "Waiting";
        retry["_cleanup"] = false;
        retry.erase("error");
        retry.erase("endedAt");
        next["jobs"].push_back(retry);
      } else
        enqueue_library(next, job.at("kind"), job.at("_options"));
    } else if (job.value("kind", "") == "service.operation")
      enqueue_service(next, job.at("_command"), job.at("_data"));
    else
      enqueue_network(next, job.at("kind"), job.value("moduleId", ""));
    next["jobs"].back()["retryOf"] = job.at("id");
  } else if (action == "jobs.clearFinished") {
    auto &jobs = next["jobs"];
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                              [](const auto &job) {
                                const auto state = job.value("state", "");
                                return state != "queued" && state != "running" &&
                                       state != "cancelling" && state != "committing" &&
                                       !job.value("_cleanup", false);
                              }),
               jobs.end());
  } else if (action == "modules.rollback") {
    auto &module = item(next["modules"], data.at("id"));
    const auto previous_version = module.value("version", "");
    auto history = module.value("previousVersions", Json::array());
    if (history.empty())
      throw std::runtime_error("No previous module version is available");
    auto previous = history.back();
    previous["compatibility"] =
        module_compatibility(manifest(fs::u8path(previous.at("path").get<std::string>())),
                             fs::u8path(previous.at("path").get<std::string>()));
    previous["schema"] = module_settings_schema(previous.at("manifest"));
    previous["settings"] =
        module_settings_values(previous.at("schema"), previous.value("settings", Json::object()));
    history.erase(history.end() - 1);
    (void)manifest(fs::u8path(previous.at("path").get<std::string>()));
    auto retained = module;
    retained.erase("previousVersions");
    history.push_back(retained);
    previous["previousVersions"] = history;
    module = previous;
    next["updateHistory"].push_back({{"id", uuid()},
                                     {"moduleId", module.at("id")},
                                     {"action", "rollback"},
                                     {"outcome", "success"},
                                     {"fromVersion", previous_version},
                                     {"toVersion", module.value("version", "")},
                                     {"message", "Previous module version restored"},
                                     {"time", now()}});
    while (next["updateHistory"].size() > 200)
      next["updateHistory"].erase(next["updateHistory"].begin());
    for (auto &game : next["games"])
      if (game.value("moduleId", "") == module.at("id").get<std::string>()) {
        game["module"] = module.at("name");
        module_metadata(game, module, next["revision"].get<std::int64_t>() + 1, false,
                        presentation_catalog_entry(next, module.at("id")));
      }
    notify(next, "Module rolled back",
           module.at("name").get<std::string>() + " " + module.at("version").get<std::string>());
  } else if (action == "modules.enable") {
    item(next["modules"], data.at("id"))["enabled"] = data.at("enabled").get<bool>();
  } else if (action == "modules.unlinkGames") {
    (void)item(next["modules"], data.at("id"));
    for (auto &game : next["games"])
      if (game.value("moduleId", "") == data.at("id").get<std::string>()) {
        game["moduleId"] = "";
        game["module"] = "No compatible module selected";
        game.erase("tileArt");
        game.erase("heroArt");
      }
    notify(next, "Module game links cleared", "The installed module and game sources were kept.");
  } else if (action == "modules.remove") {
    remove_item(next["modules"], data.at("id"));
    for (auto &game : next["games"])
      if (game.value("moduleId", "") == data.at("id").get<std::string>()) {
        game["moduleId"] = "";
        game["module"] = "No compatible module selected";
        game.erase("tileArt");
        game.erase("heroArt");
      }
  } else if (action == "modules.verify") {
    auto &module = item(next["modules"], data.at("id"));
    const auto path = fs::u8path(module.at("path").get<std::string>());
    const auto definition = manifest(path);
    if (module_identity(definition) != module.at("id").get<std::string>())
      throw std::runtime_error("Module identity changed; the registration was preserved");
    (void)module_settings_schema(definition);
    (void)normalize_dlc_definitions(definition);
    module["compatibility"] = module_compatibility(definition, path);
    const auto &report = module.at("compatibility");
    notify(next,
           report.at("issues").empty() ? "Manifest declarations checked"
                                       : "Module compatibility problems",
           report.at("message"), report.at("issues").empty() ? "info" : "warning");
  } else if (action == "modules.setting") {
    auto &module = item(next["modules"], data.at("id"));
    const auto key = data.at("key").get<std::string>();
    const auto &schema = module.at("schema");
    const auto found = std::find_if(schema.begin(), schema.end(),
                                    [&](const auto &d) { return d.value("id", "") == key; });
    if (found == schema.end())
      throw std::runtime_error("Unknown module setting");
    const auto &value = data.at("value");
    validate_module_setting(*found, value);
    module["settings"][key] = value;
  } else if (action == "modules.resetSettings") {
    auto &module = item(next["modules"], data.at("id"));
    module["settings"] = module_settings_values(module.at("schema"), Json::object());
  } else if (action == "navigation.remember" || action == "community.setPage") {
    const auto page = data.at("page").get<std::string>();
    static const std::set<std::string> pages{"Home",     "Library",    "Modules",  "Profiles",
                                             "Settings", "Support",    "Captures", "Downloads",
                                             "Network",  "Filesystem", "Developer"};
    if (!pages.contains(page))
      throw std::runtime_error("Unknown navigation destination");
    if (next.value("lastPage", "") == page)
      return project();
    next["lastPage"] = page;
  } else if (action == "notifications.push") {
    const auto title = data.at("title").get<std::string>(),
               message = data.at("message").get<std::string>();
    if (title.size() > 200 || message.size() > 2000)
      throw std::runtime_error("Notification text is too long");
    notify(next, title, message, "info", Json::object(), false);
  } else if (action == "notifications.readAll") {
    for (auto &notification : next["notifications"])
      notification["read"] = true;
  } else if (action == "notifications.dismiss") {
    remove_item(next["notifications"], data.at("id"));
  } else if (action == "notifications.read") {
    item(next["notifications"], data.at("id"))["read"] = data.value("read", true);
  } else if (action == "notifications.clear") {
    next["notifications"] = Json::array();
  } else if (action == "history.clear") {
    next["history"] = Json::array();
  } else if (action == "recovery.acknowledge") {
    recovery_.acknowledge();
  } else if (action == "system.openPath") {
    const auto target = data.at("target").get<std::string>();
    fs::path path;
    if (target == "storage")
      path = storage_;
    else if (target == "support")
      path = storage_ / "support";
    else if (target == "captures")
      path = launcher_destination(storage_, next["settings"], "paths/screenshots");
    else if (target == "location") {
      const auto id = data.at("id").get<std::string>();
      if (id == "cache" || id == "artwork" || id == "downloads") {
        const auto location = cache_location(storage_, next);
        check_cache_location(storage_, location, true);
        path = cache_root(storage_, location);
        if (id != "cache")
          path /= id == "artwork" ? "catalog-art" : "downloads";
      } else
        path = launcher_location(storage_, next["settings"], id);
      fs::create_directories(path);
    } else if (target == "game")
      path = fs::u8path(item(next["games"], data.at("id")).at("contentPath").get<std::string>());
    else if (target == "media") {
      auto &game = item(next["games"], data.at("id"));
      path = fs::u8path(game_media(game, data.at("mediaId")).at("sourcePath").get<std::string>());
    } else if (target == "managed") {
      const auto &game = item(next["games"], data.at("id"));
      const auto copies = game.value("managedCopies", Json::array());
      if (copies.empty())
        throw std::runtime_error("This game has no launcher-managed storage");
      const auto location_id = data.value("locationId", "");
      const auto copy_id = data.value("copyId", copies.front().at("entryId").get<std::string>());
      const auto copy = std::find_if(copies.begin(), copies.end(), [&](const auto &value) {
        return location_id.empty() ? value.at("entryId") == copy_id
                                   : value.at("_ownerId") == location_id;
      });
      if (copy == copies.end())
        throw std::runtime_error("The managed location no longer exists");
      path = fs::u8path(copy->at("root").get<std::string>());
    } else if (target == "dlc") {
      const auto &dlc = item(item(next["games"], data.at("id"))["dlc"], data.at("dlcId"));
      path = fs::u8path(dlc.at("path").get<std::string>());
    } else if (target == "module")
      path = fs::u8path(item(next["modules"], data.at("id")).at("path").get<std::string>());
    else if (target == "profile") {
      (void)item(next["profiles"], data.at("id"));
      path = storage_;
    } else
      throw std::runtime_error("Unknown launcher location");
    if (target == "support" || target == "captures")
      fs::create_directories(path);
    if (fs::is_regular_file(path))
      path = path.parent_path();
    if (!fs::is_directory(path))
      throw std::runtime_error("The folder no longer exists");
    open_desktop_target(path_string(path));
  } else if (action == "community.refresh") {
    presence_->refresh();
  } else if (action == "system.openUrl" || action == "community.openDiscord" || action == "community.openProject") {
    const auto info = community_information();
    const auto url = action == "community.openDiscord" ? info.at("discordUrl").get<std::string>() :
        action == "community.openProject" ? info.at("projectUrl").get<std::string>() : data.at("url").get<std::string>();
    if (url.rfind("https://", 0) != 0 || url.find_first_of("\r\n\t") != std::string::npos)
      throw std::runtime_error("Only HTTPS community links are supported");
    open_desktop_target(url);
  } else if (action == "captures.open") {
    const auto id = data.at("id").get<std::string>();
    if (id.empty() || fs::u8path(id).filename() != fs::u8path(id) ||
        id.find('\\') != std::string::npos)
      throw std::runtime_error("Invalid capture identity");
    const auto root =
        fs::weakly_canonical(launcher_destination(storage_, next["settings"], "paths/screenshots"));
    const auto path = root / fs::u8path(id);
    if (!capture_extension(path) || fs::is_symlink(path) ||
        fs::weakly_canonical(path).parent_path() != root || !fs::is_regular_file(path))
      throw std::runtime_error("Capture is missing or outside the screenshots folder");
    open_desktop_target(path_string(path));
  } else if (action == "captures.refresh") {
    captures_.refresh();
  } else if (action == "captures.delete") {
    const auto id = data.at("id").get<std::string>();
    if (id.empty() || id.find('/') != std::string::npos || id.find('\\') != std::string::npos ||
        id == "." || id == "..")
      throw std::runtime_error("Invalid capture identity");
    const auto root =
        fs::weakly_canonical(launcher_destination(storage_, next["settings"], "paths/screenshots"));
    const auto path = root / fs::u8path(id);
    const auto ext = lower(path_string(path.extension()));
    if (!capture_extension(path))
      throw std::runtime_error("Only capture images can be deleted");
    if (fs::is_symlink(path) || fs::weakly_canonical(path).parent_path() != root ||
        !fs::is_regular_file(path))
      throw std::runtime_error("Capture is missing or outside the screenshots folder");
    if (!fs::remove(path))
      throw std::runtime_error("Capture could not be deleted");
    captures_.refresh();
  } else if (action == "support.bundle") {
    const auto file = create_support_bundle(storage_, next);
    notify(next, "Support bundle created",
           "A sanitized local support ZIP was created. No files were uploaded.", "success",
           {{"type", "navigate"}, {"page", "Support"}});
  } else if (action == "application.ready") {
    recovery_.ready();
    std::ofstream(storage_ / "startup.log", std::ios::app)
        << "Launcher interface ready; bridge protocol 1\n";
  } else if (action == "session.launch") {
    if (!runtime_ || !runtime_->available())
      throw std::runtime_error("The Xenon runtime adapter is not connected");
    require_library_idle(next, data.at("gameId"));
    next["session"] = runtime_->launch(launch_configuration(next, data.at("gameId")));
    dismissed_session_.clear();
  } else if (action == "session.stop") {
    if (!runtime_)
      throw std::runtime_error("The Xenon runtime adapter is not connected");
    next["session"] = runtime_->stop();
  } else if (action == "session.dismiss") {
    if (next.contains("session")) {
      const auto value = next["session"].value("state", "");
      if (value != "failed" && value != "stopped")
        throw std::runtime_error("Session is still active");
      dismissed_session_ = next["session"].value("sessionId", "");
    }
  } else if (action.rfind("filesystem.", 0) == 0 || action.rfind("network.", 0) == 0) {
    throw std::runtime_error("The Xenon runtime adapter is not connected");
  } else
    throw std::runtime_error("This launcher action has not been converted yet: " + action);
  next["revision"] = state_["revision"].get<int64_t>() + 1;
  persist(next);
  state_ = std::move(next);
  if (!cancel_requested.empty() && running_job_ == cancel_requested && cancel_job_)
    cancel_job_->store(true);
  jobs_changed_.notify_one();
  if (command == "session.launch")
    return {{"jobId", state_.at("session").at("sessionId")}};
  return project();
}
} // namespace xenon::launcher_next
