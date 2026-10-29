#include "legacy_migration.hpp"
#include "archive.hpp"
#include "game_media.hpp"
#include "game_activity.hpp"
#include "module_settings.hpp"
#include "settings.hpp"
#include "catalogues.hpp"
#include "platform/legacy_storage.hpp"
#include "picosha2/picosha2.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <set>
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
std::string text(const fs::path &path) {
  const auto value = path.u8string();
  return {value.begin(), value.end()};
}
void cancelled(const std::shared_ptr<std::atomic_bool> &cancel) {
  if (cancel && cancel->load())
    throw OperationCancelled();
}
std::string lower(std::string value) {
  for (auto &c : value)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return value;
}
std::string prefix(const std::string &name, std::size_t max) {
  std::size_t offset = 0, units = 0;
  while (offset < name.size()) {
    const auto c = static_cast<unsigned char>(name[offset]);
    const auto bytes = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    const auto next = units + (bytes == 4 ? 2 : 1);
    if (next > max || offset + bytes > name.size())
      break;
    offset += bytes;
    units = next;
  }
  return name.substr(0, offset);
}
std::string unique_name(const Json &profiles, const std::string &original) {
  const auto first = original.find_first_not_of(" \t\r\n");
  const auto trimmed =
      first == std::string::npos
          ? std::string{}
          : original.substr(first, original.find_last_not_of(" \t\r\n") - first + 1);
  const auto base = prefix(trimmed.empty() ? "Imported profile" : trimmed, 48);
  auto value = base;
  for (unsigned i = 2;
       std::any_of(profiles.begin(), profiles.end(),
                   [&](const auto &p) { return lower(p.value("name", "")) == lower(value); });
       ++i) {
    const auto suffix = " (" + std::to_string(i) + ")";
    value = prefix(base, 48 - suffix.size()) + suffix;
  }
  return value;
}
Json read_array(const fs::path &path) {
  if (!fs::exists(path))
    return Json::array();
  auto value = read_document(path);
  if (!value.is_array())
    throw std::runtime_error("Legacy profiles/library must be JSON arrays");
  return value;
}
Json scalar(const Json &value, const Json &definition) {
  const auto type = definition.value("type", "");
  if (type == "bool" && value.is_number())
    return value.get<double>() != 0.;
  if (!value.is_string())
    return value;
  const auto string = value.get<std::string>();
  if (type == "bool") {
    const auto normalized = lower(string);
    if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on")
      return true;
    if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off" ||
        normalized.empty())
      return false;
    throw std::runtime_error("Invalid boolean");
  }
  if (type == "number") {
    std::size_t used = 0;
    const auto number = std::stod(string, &used);
    if (used != string.size() || !std::isfinite(number))
      throw std::runtime_error("Invalid number");
    return number;
  }
  return value;
}
bool image(const fs::path &file) {
  const auto ext = lower(text(file.extension()));
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp";
}
bool valid_id(const std::string &value) {
  return !value.empty() && value.size() <= 128 && value.find("..") == std::string::npos &&
         value.find_first_not_of(
             "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
             std::string::npos;
}
} // namespace
std::string migration_hash(const Json &value) { return picosha2::hash256_hex_string(value.dump()); }
std::string migration_file_hash(const fs::path &file,
                                const std::shared_ptr<std::atomic_bool> &cancel) {
  if (fs::is_symlink(file) || !fs::is_regular_file(file) || fs::file_size(file) > 20 * 1024 * 1024)
    throw std::runtime_error("Legacy avatar is missing, redirected or exceeds 20 MiB");
  std::ifstream stream(file, std::ios::binary);
  stream.exceptions(std::ios::badbit);
  if (!stream)
    throw std::runtime_error("Legacy avatar could not be read");
  picosha2::hash256_one_by_one hash;
  std::array<char, 65536> buffer;
  while (stream) {
    cancelled(cancel);
    stream.read(buffer.data(), buffer.size());
    hash.process(buffer.begin(), buffer.begin() + stream.gcount());
  }
  hash.finish();
  return picosha2::get_hash_hex_string(hash);
}
std::string migration_target_hash(const Json &state) {
  Json result = Json::object();
  for (const auto *key :
       {"profiles", "activeProfileId", "games", "modules", "settings", "preferences"})
    result[key] = state.at(key);
  return migration_hash(result);
}
Json legacy_snapshot(const fs::path &folder, const std::shared_ptr<std::atomic_bool> &cancel) {
  cancelled(cancel);
  const auto source = legacy_storage(folder);
  Json result = {{"settings", Json::object()}, {"profiles", Json::array()},
                 {"games", Json::array()},     {"modules", Json::array()},
                 {"avatars", Json::array()},   {"warnings", Json::array()},
                 {"source", text(source.app)}};
  for (const auto &definition : settings_definitions()) {
    cancelled(cancel);
    const auto key = definition.at("key").get<std::string>();
    const auto primary = source.value(key.starts_with("ui/") ? key : "frontend/" + key);
    if (primary)
      result["settings"][key] = *primary;
    else if (const auto value = source.value(key))
      result["settings"][key] = *value;
  }
  const bool found_preferences = !result["settings"].empty() ||
                                 source.value("frontend/profiles/state").has_value() ||
                                 source.value("frontend/paths/libraryRoot").has_value();
  if (const auto page = source.value("ui/page"))
    result["lastPage"] = *page;
  for (const auto &entry :
       std::vector<std::pair<std::string, fs::path>>{{"games", source.games},
                                                     {"saves", source.saves},
                                                     {"profiles", source.profiles},
                                                     {"modules", source.modules},
                                                     {"screenshots", source.screenshots},
                                                     {"libraryRoot", source.library}})
    result["settings"]["paths/" + entry.first] = text(entry.second);
  for (const auto *key : {"cache", "preparationCache"})
    if (const auto value = source.value(std::string("frontend/paths/") + key))
      result["settings"][std::string("paths/") + key] = *value;
  auto profiles = source.profiles / "profiles.json";
  if (!fs::exists(profiles) && fs::exists(source.app / "profiles.json"))
    profiles = source.app / "profiles.json";
  if (fs::exists(profiles))
    result["profiles"] = read_array(profiles);
  else if (const auto value = source.value("frontend/profiles/state");
           value && value->is_string() && !value->get<std::string>().empty()) {
    result["profiles"] = Json::parse(value->get<std::string>());
    if (!result["profiles"].is_array())
      throw std::runtime_error("Invalid previous profile settings");
  }
  auto library = source.app / "Library/library.json";
  if (!fs::exists(library))
    library = source.app / "library.json";
  result["games"] = read_array(library);
  if (result["profiles"].size() > 512 || result["games"].size() > 10000)
    throw std::runtime_error(
        "Legacy data exceeds the migration limits (512 profiles / 10,000 games)");
  for (std::size_t i = 0; i < result["profiles"].size(); ++i) {
    cancelled(cancel);
    const auto &profile = result["profiles"][i];
    if (!profile.is_object())
      throw std::runtime_error("Invalid legacy profile record");
    const auto url = profile.value("avatarPath", "");
    if (url.empty())
      continue;
    try {
      auto file = legacy_local_file(url);
      if (file.is_relative())
        file = source.profiles / file;
      if (!image(file))
        throw std::runtime_error("Unsupported legacy avatar image type");
      result["avatars"].push_back({{"profile", i},
                                   {"path", text(fs::absolute(file).lexically_normal())},
                                   {"sha256", migration_file_hash(file, cancel)}});
    } catch (const OperationCancelled &) {
      throw;
    } catch (const std::exception &error) {
      result["warnings"].push_back("Avatar for profile " + std::to_string(i + 1) + ": " +
                                   error.what());
    }
  }
  if (fs::is_directory(source.modules)) {
    unsigned count = 0;
    for (const auto &entry : fs::directory_iterator(source.modules)) {
      cancelled(cancel);
      if (++count > 2048)
        throw std::runtime_error("Too many entries in the previous module folder");
      if (entry.is_symlink() || !entry.is_directory())
        continue;
      for (const auto *name : {"xenon-module.json", "module.json", "manifest.json"})
        if (fs::is_regular_file(entry.path() / name)) {
          try {
            const auto manifest = read_document(entry.path() / name, 1024 * 1024);
            const auto id =
                manifest.value("id", manifest.value("moduleId", manifest.value("module_id", "")));
            Json values = Json::object();
            const auto schema = module_settings_schema(manifest);
            for (const auto &definition : schema) {
              const auto key = definition.at("id").get<std::string>();
              if (const auto value = source.value("modules/settings/" + id + "/" + key))
                values[key] = scalar(*value, definition);
            }
            const auto enabled = source.value("modules/enabled/" + id);
            result["modules"].push_back(
                {{"manifest", manifest},
                 {"path", text(fs::weakly_canonical(entry.path()))},
                 {"settings", values},
                 {"enabled", enabled ? scalar(*enabled, {{"type", "bool"}}) : Json(true)}});
          } catch (const std::exception &error) {
            result["warnings"].push_back("Module " + text(entry.path().filename()) +
                                         " was skipped: " + error.what());
          }
          break;
        }
    }
  }
  if (result["modules"].size() > 256)
    throw std::runtime_error("More than 256 previous modules; choose a smaller copied data folder");
  std::sort(result["modules"].begin(), result["modules"].end(),
            [](const auto &a, const auto &b) { return a.at("path") < b.at("path"); });
  if (!found_preferences && !fs::is_regular_file(profiles) && !fs::is_regular_file(library) &&
      result["modules"].empty())
    throw std::runtime_error(
        "No previous launcher settings, profiles, library or modules were found in this location");
  std::sort(result["warnings"].begin(), result["warnings"].end());
  result["fingerprint"] = migration_hash(result);
  return result;
}
Json legacy_plan(const Json &input, const Json &state, const std::string &id, bool replace_settings,
                 bool activate) {
  auto next = state;
  Json warnings = input.at("warnings"), counts = {{"profiles", 0}, {"games", 0},   {"modules", 0},
                                                  {"settings", 0}, {"avatars", 0}, {"skipped", 0}};
  const auto origin = migration_hash(input.at("source"));
  if (input.contains("lastPage") && (replace_settings || !state.contains("lastPage"))) {
    try {
      const auto page = input.at("lastPage").is_string()
                            ? std::stoi(input.at("lastPage").get<std::string>())
                            : input.at("lastPage").get<int>();
      const std::vector<std::string> pages{"Library", "Modules",   "Profiles", "Settings",
                                           "Home",    "Downloads", "Captures", "Network",
                                           "Support", "Settings"};
      if (page >= 0 && page < static_cast<int>(pages.size()))
        next["lastPage"] = pages[page];
    } catch (const std::exception &) {
      warnings.push_back("The previous remembered page was invalid and was skipped.");
    }
  }
  const auto warn = [&](const std::string &message) { warnings.push_back(message); };
  for (auto field = input.at("settings").begin(); field != input.at("settings").end(); ++field) {
    if (!replace_settings && next["settings"].contains(field.key())) {
      counts["skipped"] = counts["skipped"].get<int>() + 1;
      continue;
    }
    try {
      Json value = field.value();
      for (const auto &definition : settings_definitions())
        if (definition.at("key") == field.key())
          value = scalar(value, definition);
      validate_setting(field.key(), value);
      next["settings"][field.key()] = value;
      counts["settings"] = counts["settings"].get<int>() + 1;
      if (field.key() == "general/startupPage")
        next["preferences"]["startPage"] = value;
      if (field.key() == "accessibility/reduceMotion")
        next["preferences"]["reducedMotion"] = value;
    } catch (const std::exception &error) {
      warn("Setting " + field.key() + " was skipped: " + error.what());
    }
  }
  Json copies = Json::array();
  for (std::size_t i = 0; i < input.at("profiles").size(); ++i) {
    const auto &source = input["profiles"][i];
    const auto legacy = source.value("profileId", "profile-" + std::to_string(i));
    auto found =
        std::find_if(next["profiles"].begin(), next["profiles"].end(), [&](const auto &row) {
          return row.value("legacySource", "") == origin && row.value("legacyId", "") == legacy;
        });
    if (found != next["profiles"].end()) {
      if (activate && source.value("active", false))
        next["activeProfileId"] = found->at("id");
      counts["skipped"] = counts["skipped"].get<int>() + 1;
      continue;
    }
    try {
      const auto profile_id = make_id();
      Json profile = {
          {"id", profile_id},
          {"name", unique_name(next["profiles"], source.value("profileName", "Imported profile"))},
          {"description", prefix(source.value("description", ""), 180)},
          {"legacySource", origin},
          {"legacyId", legacy},
          {"legacyMetadata", source},
          {"offline", source.value("offline", true)},
          {"isolatedSettings", source.value("isolatedSettings", false)},
          {"runtimeOverrides", source.value("runtimeOverrides", Json::object())},
          {"region", source.value("region", "Auto (Global)")},
          {"startupPage", source.value("startupPage", "Launcher default")}};
      for (const auto *key : {"gamePath", "savePath", "screenshotPath"})
        profile[key] = source.value(key, "");
      for (const auto *key : {"avatarFocalX", "avatarFocalY", "avatarZoom"}) {
        const double value = source.value(key, std::string(key) == "avatarZoom" ? 1. : .5);
        profile[key] = std::isfinite(value)
                           ? std::clamp(value, std::string(key) == "avatarZoom" ? 1. : 0.,
                                        std::string(key) == "avatarZoom" ? 3. : 1.)
                           : 1.;
      }
      normalize_profile_preferences(profile, next["settings"]);
      for (const auto &avatar : input.at("avatars"))
        if (avatar.at("profile") == i) {
          const auto key =
              "legacy-" + id + "-" + std::to_string(i) +
              lower(text(fs::u8path(avatar.at("path").get<std::string>()).extension()));
          profile["avatarKey"] = key;
          profile["avatarPath"] = "xenon://app/avatars/" + profile_id;
          auto copy = avatar;
          copy["key"] = key;
          copies.push_back(copy);
          counts["avatars"] = counts["avatars"].get<int>() + 1;
        }
      next["profiles"].push_back(profile);
      counts["profiles"] = counts["profiles"].get<int>() + 1;
      if (activate && source.value("active", false))
        next["activeProfileId"] = profile_id;
    } catch (const std::exception &error) {
      warn("Profile " + std::to_string(i + 1) + " was skipped: " + error.what());
    }
  }
  for (const auto &source : input.at("modules")) {
    try {
      const auto &manifest = source.at("manifest");
      const auto module_id =
          manifest.value("id", manifest.value("moduleId", manifest.value("module_id", "")));
      if (!valid_id(module_id))
        throw std::runtime_error("Invalid module identifier");
      if (std::any_of(next["modules"].begin(), next["modules"].end(),
                      [&](const auto &m) { return m.at("id") == module_id; })) {
        counts["skipped"] = counts["skipped"].get<int>() + 1;
        continue;
      }
      const auto schema = module_settings_schema(manifest);
      next["modules"].push_back(
          {{"id", module_id},
           {"name", manifest.value("name", manifest.value("title", module_id))},
           {"version", manifest.value("version", "Unknown")},
           {"path", source.at("path")},
           {"enabled", source.at("enabled")},
           {"schema", schema},
           {"settings", module_settings_values(schema, source.at("settings"))},
           {"manifest", manifest},
           {"managed", false},
           {"legacySource", origin}});
      counts["modules"] = counts["modules"].get<int>() + 1;
    } catch (const std::exception &error) {
      warn("A module was skipped: " + std::string(error.what()));
    }
  }
  for (const auto &source : input.at("games")) {
    try {
      const auto path = source.value("contentPath", "");
      if (path.empty())
        throw std::runtime_error("Missing content path");
      if (std::any_of(next["games"].begin(), next["games"].end(),
                      [&](const auto &g) { return g.value("contentPath", "") == path; })) {
        counts["skipped"] = counts["skipped"].get<int>() + 1;
        continue;
      }
      const auto module_id = source.value("moduleId", "");
      const auto found = std::find_if(next["modules"].begin(), next["modules"].end(),
                                      [&](const auto &m) { return m.at("id") == module_id; });
      Json game = {
          {"id", make_id()},
          {"title", source.value("title", "Imported game")},
          {"description", source.value("description", "")},
          {"favorite", source.value("favorite", false)},
          {"moduleId", found == next["modules"].end() ? "" : module_id},
          {"module", found == next["modules"].end() ? "Unassigned" : found->value("name", "")},
          {"contentPath", path},
          {"lastPlayed",
           source.value("lastPlayedAt", "").empty() ? Json() : source.at("lastPlayedAt")},
          {"addedAt", source.value("addedAt", "")},
          {"verified", false},
          {"dlc", Json::array()},
          {"legacyMetadata", source},
          {"legacySource", origin},
          {"legacyId", source.value("gameId", "")}};
      for (const auto &dlc : source.value("dlc", Json::array()))
        if (dlc.is_object()) {
          const auto dlc_path = dlc.value("contentPath", dlc.value("path", ""));
          if (!dlc_path.empty())
            game["dlc"].push_back({{"id", make_id()},
                                   {"name", dlc.value("name", "Imported add-on")},
                                   {"path", dlc_path},
                                   {"catalogId", dlc.value("dlcId", dlc.value("catalogId", ""))},
                                   {"verified", false},
                                   {"legacyMetadata", dlc}});
        }
      if (!fs::exists(fs::u8path(path)))
        warn("Source is currently missing for " + game.at("title").get<std::string>() +
             "; its reference will be retained.");
      normalize_game_media(game);
      normalize_game_activity(game);
      next["games"].push_back(game);
      counts["games"] = counts["games"].get<int>() + 1;
    } catch (const std::exception &error) {
      warn("A library entry was skipped: " + std::string(error.what()));
    }
  }
  Json patch = Json::object();
  for (const auto *key :
       {"profiles", "activeProfileId", "games", "modules", "settings", "preferences"})
    patch[key] = next.at(key);
  if (next.contains("lastPage"))
    patch["lastPage"] = next.at("lastPage");
  Json changes = {{"profiles", Json::array()},
                  {"games", Json::array()},
                  {"modules", Json::array()},
                  {"settings", Json::array()}};
  for (const auto *collection : {"profiles", "games", "modules"})
    for (const auto &entry : next.at(collection))
      if (std::none_of(state.at(collection).begin(), state.at(collection).end(),
                       [&](const auto &old) { return old.at("id") == entry.at("id"); }))
        changes[collection].push_back(
            entry.value(std::string(collection) == "games" ? "title" : "name", "Imported entry"));
  for (auto value = next.at("settings").begin(); value != next.at("settings").end(); ++value)
    if (!state.at("settings").contains(value.key()) ||
        state.at("settings").at(value.key()) != value.value())
      changes["settings"].push_back(value.key() + " = " + value.value().dump());
  return {{"schema", "xenon.legacy-migration-plan"},
          {"version", 1},
          {"id", id},
          {"sourceHash", input.at("fingerprint")},
          {"targetHash", migration_target_hash(state)},
          {"patch", patch},
          {"avatars", copies},
          {"summary",
           {{"id", id},
            {"state", "review"},
            {"source", input.at("source")},
            {"counts", counts},
            {"warnings", warnings},
            {"changes", changes},
            {"replaceSettings", replace_settings},
            {"activateProfile", activate}}}};
}
} // namespace xenon::launcher_next
