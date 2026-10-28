#include "launcher_service.hpp"
#include "core/archive.hpp"
#include "platform/desktop.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
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
void module_metadata(Json &game, const Json &module, std::int64_t revision, bool title = false) {
  auto metadata = module.at("manifest").value("launcher", Json::object());
  if (!metadata.is_object())
    metadata = Json::object();
  for (const auto *key : {"tileArt", "heroArt"}) {
    if (metadata.contains(key) && metadata[key].is_string() &&
        !metadata[key].get<std::string>().empty())
      game[key] = std::string("xenon://app/") +
                  (std::string(key) == "tileArt" ? "game-tile/" : "game-hero/") +
                  game.at("id").get<std::string>() + "?v=" + std::to_string(revision);
    else
      game.erase(key);
  }
  if (title && metadata.contains("gameTitle") && metadata["gameTitle"].is_string() &&
      !trim(metadata["gameTitle"].get<std::string>()).empty())
    game["title"] = trim(metadata["gameTitle"].get<std::string>());
  if (metadata.contains("description") && metadata["description"].is_string())
    game["description"] = metadata["description"];
  for (const auto *key : {"tileArtFocalX", "tileArtFocalY", "heroArtFocalX", "heroArtFocalY"}) {
    game.erase(key);
    if (metadata.contains(key) && metadata[key].is_number())
      game[key] = std::clamp(metadata[key].get<double>(), 0., 1.);
  }
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
  auto candidate = name_prefix(base, 40);
  for (unsigned suffix = 2; std::any_of(
           profiles.begin(), profiles.end(),
           [&](const auto &row) { return lower(row.value("name", "")) == lower(candidate); });
       ++suffix) {
    const auto ending = " (" + std::to_string(suffix) + ")";
    candidate = name_prefix(base, 40 - ending.size()) + ending;
  }
  return candidate;
}
void validate_name(const Json &profiles, const std::string &name, const std::string &exclude = "") {
  if (name.empty() || name_prefix(name, 40) != name)
    throw std::runtime_error("Enter a profile name of at most 40 characters");
  for (const auto &profile : profiles)
    if (profile.at("id") != exclude && lower(profile.at("name")) == lower(name))
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
LauncherService::LauncherService(fs::path storage, bool safe_mode, fs::path fixture_host)
    : storage_(std::move(storage)), safe_mode_(safe_mode) {
  load();
  recovery_needed_ = fs::exists(storage_ / "running.json");
  atomic_json(storage_ / "running.json", {{"startedAt", now()}});
  if (!safe_mode_ && !fixture_host.empty())
    runtime_ = fixture_runtime(std::move(fixture_host), storage_);
}
LauncherService::~LauncherService() {
  std::error_code ignored;
  fs::remove(storage_ / "running.json", ignored);
}
void LauncherService::load() {
  const auto file = storage_ / "state.json";
  if (fs::exists(file)) {
    state_ = read_json(file);
    validate(state_);
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
}
void LauncherService::notify(Json &next, const std::string &title, const std::string &message) {
  next["notifications"].push_back(
      {{"id", uuid()}, {"title", title}, {"message", message}, {"read", false}, {"time", now()}});
  if (next["notifications"].size() > 200)
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
Json LauncherService::project() const {
  auto result = state_;
  result["mode"] = "native";
  result["capabilities"] = {{"runtime", runtime_ && runtime_->available()},
                            {"importContent", true},
                            {"modules", true},
                            {"captures", true}};
  if (!runtime_)
    result["session"] = {{"state", "unavailable"}, {"fixture", false}};
  else if (result.contains("session") &&
           result["session"].value("sessionId", "") == dismissed_session_)
    result["session"] = {{"state", "idle"}, {"fixture", true}};
  result["storagePath"] = path_string(storage_);
  result["recovery"] = {{"previousUncleanExit", recovery_needed_}, {"safeMode", safe_mode_}};
  result["captures"] = Json::array();
  const auto capture_path =
      fs::u8path(state_["settings"].value("paths/screenshots", path_string(storage_ / "captures")));
  if (fs::is_directory(capture_path)) {
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(capture_path, error)) {
      const auto ext = lower(path_string(entry.path().extension()));
      if (entry.is_regular_file(error) &&
          (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp"))
        result["captures"].push_back({{"id", path_string(entry.path().filename())},
                                      {"name", path_string(entry.path().filename())},
                                      {"size", entry.file_size(error)}});
      if (result["captures"].size() >= 10000)
        break;
    }
  }
  return result;
}
void LauncherService::poll_runtime() {
  if (!runtime_)
    return;
  auto report = runtime_->status();
  const auto previous = state_.value("session", Json::object());
  state_["session"] = report;
  const auto current = report.value("state", "");
  const auto id = report.value("sessionId", "");
  bool changed = previous.value("state", "") != current || previous.value("sessionId", "") != id;
  if ((current == "stopped" || current == "failed") && !id.empty() &&
      std::none_of(state_["history"].begin(), state_["history"].end(),
                   [&](const auto &h) { return h.value("id", "") == id; })) {
    state_["history"].push_back(
        {{"id", id},
         {"title", report.value("title", "Test session")},
         {"time", std::to_string(report.value("endedAtEpochMs", std::int64_t{}))},
         {"state", current},
         {"fixture", true}});
    changed = true;
    if (current == "failed")
      notify(state_, "Fixture session failed", report.value("lastError", "Unknown failure"));
  }
  if (changed) {
    state_["revision"] = state_["revision"].get<std::int64_t>() + 1;
    persist(state_);
  }
}
Json LauncherService::launch_configuration(const Json &state, const std::string &game_id) const {
  auto games = state.at("games"), profiles = state.at("profiles");
  const auto &game = item(games, game_id);
  const auto &profile = item(profiles, state.at("activeProfileId"));
  const auto source = game.value("contentPath", "");
  if (!fs::exists(fs::u8path(source)))
    throw std::runtime_error("Game source is missing");
  const auto &settings = state.at("settings");
  Json config = {{"gameId", game_id},
                 {"title", game.at("title")},
                 {"contentPath", source},
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
                           std::pair{"screenshotPath", "screenshotsPath"}})
    config[pair.second] = profile.value(pair.first, "");
  const auto module_id = game.value("moduleId", "");
  if (!module_id.empty()) {
    auto modules = state.at("modules");
    const auto &module = item(modules, module_id);
    if (!module.value("enabled", true))
      throw std::runtime_error("The selected module is disabled");
    config["moduleName"] = module.at("name");
    config["modulePath"] = module.at("path");
    config["moduleVersion"] = module.at("version");
    config["moduleSettings"] = module.at("settings");
    config["runtimeApiRequirements"] = module.at("manifest").value("runtimeApis", Json::object());
  }
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
  if (category == "captures") {
    root = fs::weakly_canonical(fs::u8path(
        state_["settings"].value("paths/screenshots", path_string(storage_ / "captures"))));
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
  if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".webp")
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
      result = execute(request.at("command"), request.at("payload"));
    }
    return {{"version", 1}, {"id", id}, {"ok", true}, {"data", result}};
  } catch (const std::exception &e) {
    return {{"version", 1},
            {"id", id},
            {"ok", false},
            {"error", {{"code", "COMMAND_FAILED"}, {"message", e.what()}}}};
  }
}
Json LauncherService::execute(const std::string &command, const Json &payload) {
  auto next = state_;
  const auto action =
      command == "launcher.action" ? payload.at("action").get<std::string>() : command;
  const auto data = command == "launcher.action" ? payload.value("data", Json::object()) : payload;
  if (action == "profiles.create" || action == "profiles.update" ||
      action == "profiles.duplicate") {
    const auto id = data.value("id", "");
    Json profile = action == "profiles.create" ? Json{{"id", uuid()},
                                                      {"description", ""},
                                                      {"region", "Auto (Global)"},
                                                      {"offline", true},
                                                      {"paths", Json::object()}}
                                               : item(next["profiles"], id);
    auto name = trim(data.value("name", profile.value("name", "")));
    if (action == "profiles.duplicate") {
      name = unique_profile_name(next["profiles"], name_prefix(name, 35) + " copy");
      profile["id"] = uuid();
    }
    validate_name(next["profiles"], name, action == "profiles.update" ? id : "");
    profile["name"] = name;
    for (const auto *key :
         {"description", "region", "startupPage", "gamePath", "savePath", "screenshotPath"})
      if (data.contains(key)) {
        if (!data[key].is_string())
          throw std::runtime_error("Invalid profile field");
        profile[key] = data[key];
      }
    for (const auto *key : {"avatarFocalX", "avatarFocalY", "avatarZoom"})
      if (data.contains(key)) {
        const auto value = data[key].get<double>();
        if (value < (std::string(key) == "avatarZoom" ? 1.0 : 0.0) ||
            value > (std::string(key) == "avatarZoom" ? 4.0 : 1.0))
          throw std::runtime_error("Invalid avatar crop");
        profile[key] = value;
      }
    for (const auto *key : {"offline", "isolatedSettings"})
      if (data.contains(key)) {
        if (!data[key].is_boolean())
          throw std::runtime_error("Invalid profile flag");
        profile[key] = data[key];
      }
    if (action == "profiles.update")
      item(next["profiles"], id) = profile;
    else
      next["profiles"].push_back(profile);
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
    const auto base = trim(source.value("profileName", source.value("name", "Imported profile")));
    const auto name = unique_profile_name(next["profiles"], base);
    validate_name(next["profiles"], name);
    Json profile = {{"id", uuid()},
                    {"name", name},
                    {"description", source.value("description", "")},
                    {"region", source.value("region", "Auto (Global)")},
                    {"offline", source.value("offline", true)},
                    {"isolatedSettings", source.value("isolatedSettings", false)},
                    {"paths", Json::object()}};
    next["profiles"].push_back(profile);
  } else if (action == "settings.update") {
    for (const auto &[key, value] : data.items()) {
      if (key != "theme" && key != "reducedMotion" && key != "startPage")
        throw std::runtime_error("Unknown preference");
      next["preferences"][key] = value;
    }
  } else if (action == "settings.set") {
    const auto key = data.at("key").get<std::string>();
    auto schema = read_json(storage_ / "settings-schema.json");
    const auto found = std::find_if(schema.begin(), schema.end(),
                                    [&](const auto &entry) { return entry.at("key") == key; });
    if (found == schema.end() && key.rfind("paths/", 0) != 0)
      throw std::runtime_error("Unknown setting");
    const auto &value = data.at("value");
    if (found != schema.end()) {
      const auto type = found->at("type");
      if ((type == "bool" && !value.is_boolean()) || (type == "number" && !value.is_number()) ||
          ((type == "enum" || type == "string") && !value.is_string()))
        throw std::runtime_error("Invalid setting type");
      if (type == "enum" && std::none_of((*found)["options"].begin(), (*found)["options"].end(),
                                         [&](const auto &o) { return o.at("value") == value; }))
        throw std::runtime_error("Invalid setting option");
      if (found->contains("minimum") &&
          (value < found->at("minimum") || value > found->at("maximum")))
        throw std::runtime_error("Setting outside permitted range");
    } else if (!value.is_string())
      throw std::runtime_error("Invalid path");
    if (key == "ui/customAccent") {
      const auto color = value.get<std::string>();
      if (color.size() != 7 || color[0] != '#' ||
          color.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
        throw std::runtime_error("Use a six-digit hexadecimal accent colour");
    }
    next["settings"][key] = value;
    if (key == "general/startupPage")
      next["preferences"]["startPage"] = value;
    if (key == "accessibility/reduceMotion")
      next["preferences"]["reducedMotion"] = value;
  } else if (action == "settings.reset") {
    const auto category = data.value("category", "");
    std::set<std::string> keys;
    if (fs::exists(storage_ / "settings-schema.json"))
      for (const auto &definition : read_json(storage_ / "settings-schema.json"))
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
  } else if (action == "library.import") {
    for (const auto &source : data.at("paths")) {
      const auto path = fs::weakly_canonical(fs::u8path(source.get<std::string>()));
      if (!fs::exists(path))
        throw std::runtime_error("Game source does not exist");
      const auto text = path_string(path);
      if (std::any_of(next["games"].begin(), next["games"].end(),
                      [&](const auto &g) { return g.value("contentPath", "") == text; }))
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
    }
    notify(next, "Library updated",
           "Local sources registered; compatibility has not been verified.");
  } else if (action == "library.favorite") {
    item(next["games"], data.at("id"))["favorite"] = data.at("favorite").get<bool>();
  } else if (action == "library.remove") {
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
      const auto id = data.at("moduleId").get<std::string>();
      if (id.empty()) {
        game["moduleId"] = "";
        game["module"] = "No compatible module selected";
        game.erase("tileArt");
        game.erase("heroArt");
      } else {
        auto &mod = item(next["modules"], id);
        game["moduleId"] = mod.at("id");
        game["module"] = mod.at("name");
        module_metadata(game, mod, next["revision"].get<std::int64_t>() + 1);
      }
    }
  } else if (action == "library.refreshMetadata") {
    auto &game = item(next["games"], data.at("id"));
    const auto &module = item(next["modules"], game.value("moduleId", ""));
    module_metadata(game, module, next["revision"].get<std::int64_t>() + 1, true);
  } else if (action == "library.verify") {
    auto &game = item(next["games"], data.at("id"));
    if (!fs::exists(fs::u8path(game.at("contentPath").get<std::string>())))
      throw std::runtime_error("The registered source is missing");
    notify(next, "Source exists",
           "The source path exists. Game integrity and compatibility are not "
           "validated without the content adapter.");
  } else if (action == "dlc.import") {
    auto &game = item(next["games"], data.at("id"));
    for (const auto &source : data.at("paths")) {
      const auto path = fs::u8path(source.get<std::string>());
      if (!fs::exists(path))
        throw std::runtime_error("DLC source is missing");
      game["dlc"].push_back({{"id", uuid()},
                             {"name", path_string(path.filename())},
                             {"path", path_string(path)},
                             {"verified", false}});
    }
  } else if (action == "dlc.remove") {
    remove_item(item(next["games"], data.at("id"))["dlc"], data.at("dlcId"));
  } else if (action == "modules.import") {
    const auto folder = fs::weakly_canonical(fs::u8path(data.at("path").get<std::string>()));
    const auto definition = manifest(folder);
    const auto id = definition.value("id", definition.value("moduleId", ""));
    if (id.empty())
      throw std::runtime_error("Module manifest has no ID");
    if (std::any_of(next["modules"].begin(), next["modules"].end(),
                    [&](const auto &m) { return m.at("id") == id; }))
      throw std::runtime_error("This module is already registered");
    next["modules"].push_back({{"id", id},
                               {"name", definition.value("name", id)},
                               {"version", definition.value("version", "Unknown")},
                               {"path", path_string(folder)},
                               {"enabled", true},
                               {"schema", definition.value("settings", Json::array())},
                               {"settings", Json::object()},
                               {"manifest", definition}});
    notify(next, "Module registered", definition.value("name", id));
  } else if (action == "modules.installPackage") {
    const auto source = fs::u8path(data.at("path").get<std::string>());
    const auto stage = storage_ / "staging" / uuid();
    fs::path package_root;
    try {
      extract_package(source, stage);
      std::vector<fs::path> candidates;
      for (const auto &entry : fs::recursive_directory_iterator(stage)) {
        const auto name = path_string(entry.path().filename());
        if (entry.is_regular_file() &&
            (name == "xenon-module.json" || name == "module.json" || name == "manifest.json")) {
          if (std::find(candidates.begin(), candidates.end(), entry.path().parent_path()) ==
              candidates.end())
            candidates.push_back(entry.path().parent_path());
        }
      }
      if (candidates.size() != 1)
        throw std::runtime_error("Package must contain exactly one module root");
      package_root = candidates.front();
      const auto definition = manifest(package_root);
      const auto id = definition.value("id", definition.value("moduleId", ""));
      if (id.empty())
        throw std::runtime_error("Module package manifest has no ID");
      if (definition.contains("settings") && !definition.at("settings").is_array())
        throw std::runtime_error("Invalid module settings schema");
      const auto installed =
          fs::u8path(next["settings"].value("paths/modules", path_string(storage_ / "modules"))) /
          uuid();
      fs::create_directories(installed.parent_path());
      const auto relative = package_root.lexically_relative(stage);
      fs::rename(stage, installed);
      Json replacement = {{"id", id},
                          {"name", definition.value("name", id)},
                          {"version", definition.value("version", "Unknown")},
                          {"path", path_string(installed / relative)},
                          {"enabled", true},
                          {"schema", definition.value("settings", Json::array())},
                          {"settings", Json::object()},
                          {"manifest", definition},
                          {"managed", true},
                          {"previousVersions", Json::array()}};
      auto previous = std::find_if(next["modules"].begin(), next["modules"].end(),
                                   [&](const auto &module) { return module.at("id") == id; });
      if (previous != next["modules"].end()) {
        auto history = previous->value("previousVersions", Json::array());
        auto backup = *previous;
        backup.erase("previousVersions");
        history.push_back(backup);
        if (history.size() > 5)
          history.erase(history.begin());
        replacement["previousVersions"] = history;
        replacement["settings"] = previous->value("settings", Json::object());
        replacement["enabled"] = previous->value("enabled", true);
        *previous = replacement;
      } else
        next["modules"].push_back(replacement);
      next["jobs"].push_back({{"id", uuid()},
                              {"title", "Install " + definition.value("name", id)},
                              {"state", "completed"},
                              {"progress", 100}});
      notify(next, "Module package installed",
             definition.value("name", id) + " " + definition.value("version", "Unknown"));
      for (auto &game : next["games"])
        if (game.value("moduleId", "") == id) {
          game["module"] = replacement.at("name");
          module_metadata(game, replacement, next["revision"].get<std::int64_t>() + 1);
        }
    } catch (...) {
      // Stage is generated below our private staging root, never a user source.
      if (stage.parent_path() == storage_ / "staging") {
        std::error_code ignored;
        fs::remove_all(stage, ignored);
      }
      throw;
    }
  } else if (action == "modules.rollback") {
    auto &module = item(next["modules"], data.at("id"));
    auto history = module.value("previousVersions", Json::array());
    if (history.empty())
      throw std::runtime_error("No previous module version is available");
    auto previous = history.back();
    history.erase(history.end() - 1);
    (void)manifest(fs::u8path(previous.at("path").get<std::string>()));
    previous["previousVersions"] = history;
    module = previous;
    for (auto &game : next["games"])
      if (game.value("moduleId", "") == module.at("id").get<std::string>()) {
        game["module"] = module.at("name");
        module_metadata(game, module, next["revision"].get<std::int64_t>() + 1);
      }
    notify(next, "Module rolled back",
           module.at("name").get<std::string>() + " " + module.at("version").get<std::string>());
  } else if (action == "modules.enable") {
    item(next["modules"], data.at("id"))["enabled"] = data.at("enabled").get<bool>();
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
    (void)manifest(fs::u8path(item(next["modules"], data.at("id")).at("path").get<std::string>()));
    notify(next, "Manifest readable",
           "Native module binaries and runtime APIs have not been validated.");
  } else if (action == "modules.setting") {
    auto &module = item(next["modules"], data.at("id"));
    const auto key = data.at("key").get<std::string>();
    const auto &schema = module.at("schema");
    const auto found = std::find_if(schema.begin(), schema.end(),
                                    [&](const auto &d) { return d.value("id", "") == key; });
    if (found == schema.end())
      throw std::runtime_error("Unknown module setting");
    const auto type = found->value("type", "string");
    const auto &value = data.at("value");
    if ((type == "bool" || type == "boolean")
            ? !value.is_boolean()
            : (type == "number" ? !value.is_number() : !value.is_string()))
      throw std::runtime_error("Invalid module setting type");
    module["settings"][key] = value;
  } else if (action == "notifications.read") {
    item(next["notifications"], data.at("id"))["read"] = data.value("read", true);
  } else if (action == "notifications.clear") {
    next["notifications"] = Json::array();
  } else if (action == "history.clear") {
    next["history"] = Json::array();
  } else if (action == "recovery.acknowledge") {
    recovery_needed_ = false;
  } else if (action == "system.openPath") {
    const auto target = data.at("target").get<std::string>();
    fs::path path;
    if (target == "storage")
      path = storage_;
    else if (target == "support")
      path = storage_ / "support";
    else if (target == "captures")
      path = fs::u8path(
          next["settings"].value("paths/screenshots", path_string(storage_ / "captures")));
    else if (target == "game")
      path = fs::u8path(item(next["games"], data.at("id")).at("contentPath").get<std::string>());
    else if (target == "module")
      path = fs::u8path(item(next["modules"], data.at("id")).at("path").get<std::string>());
    else
      throw std::runtime_error("Unknown launcher location");
    if (target == "support" || target == "captures")
      fs::create_directories(path);
    if (fs::is_regular_file(path))
      path = path.parent_path();
    if (!fs::is_directory(path))
      throw std::runtime_error("The folder no longer exists");
    open_desktop_target(path_string(path));
  } else if (action == "system.openUrl") {
    const auto url = data.at("url").get<std::string>();
    if (url.rfind("https://", 0) != 0 || url.find_first_of("\r\n\t") != std::string::npos)
      throw std::runtime_error("Only HTTPS community links are supported");
    open_desktop_target(url);
  } else if (action == "captures.delete") {
    const auto id = data.at("id").get<std::string>();
    if (id.empty() || id.find('/') != std::string::npos || id.find('\\') != std::string::npos ||
        id == "." || id == "..")
      throw std::runtime_error("Invalid capture identity");
    const auto root = fs::weakly_canonical(fs::u8path(
        next["settings"].value("paths/screenshots", path_string(storage_ / "captures"))));
    const auto path = root / fs::u8path(id);
    const auto ext = lower(path_string(path.extension()));
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".webp")
      throw std::runtime_error("Only capture images can be deleted");
    if (fs::is_symlink(path) || fs::weakly_canonical(path).parent_path() != root ||
        !fs::is_regular_file(path))
      throw std::runtime_error("Capture is missing or outside the screenshots folder");
    if (!fs::remove(path))
      throw std::runtime_error("Capture could not be deleted");
  } else if (action == "support.bundle") {
    const auto folder = storage_ / "support" / now();
    Json report = {{"launcher", "Xenon Launcher Next"},       {"protocolVersion", 1},
                   {"profileCount", next["profiles"].size()}, {"gameCount", next["games"].size()},
                   {"moduleCount", next["modules"].size()},   {"runtimeConnected", false}};
    atomic_json(folder / "diagnostics.json", report);
    write_zip(folder / "xenon-support.zip",
              {{"diagnostics.json", report.dump(2)},
               {"README.txt", "Xenon Launcher Next support summary. No profile "
                              "names, local paths, game files, saves or logs are "
                              "included. Nothing was uploaded automatically.\n"}});
    notify(next, "Support bundle created",
           "A local support ZIP was created under the launcher support "
           "directory. No files were uploaded.");
  } else if (action == "session.launch") {
    if (!runtime_ || !runtime_->available())
      throw std::runtime_error("The Xenon runtime adapter is not connected");
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
  if (command == "session.launch")
    return {{"jobId", state_.at("session").at("sessionId")}};
  return project();
}
} // namespace xenon::launcher_next
