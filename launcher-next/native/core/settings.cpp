#include "settings.hpp"
#include "catalogues.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <regex>
namespace xenon::launcher_next {
void validate_setting(const std::string &key, const Json &value, bool presence_available) {
  if (key == "general/language") {
    if (!value.is_string() ||
        (value != "auto" && !std::regex_match(value.get<std::string>(),
                                              std::regex("^[a-z]{2,3}(?:-[A-Za-z0-9]{2,8})*$"))))
      throw std::runtime_error("Choose an available language or the system default");
  }
  if (key == "community/discordRichPresence" && value != false && !presence_available)
    throw std::runtime_error(
        "Discord Rich Presence requires an available provider and a new explicit opt-in");
  const auto &schema = settings_definitions();
  const auto found = std::find_if(schema.begin(), schema.end(), [&](const auto &definition) {
    return definition.at("key") == key;
  });
  if (found == schema.end()) {
    static const std::set<std::string> paths{
        "paths/games",       "paths/saves", "paths/profiles",    "paths/modules",
        "paths/screenshots", "paths/cache", "paths/libraryRoot", "paths/preparationCache"};
    if (!paths.contains(key) || !value.is_string())
      throw std::runtime_error("Unknown setting or invalid path value");
    return;
  }
  const auto type = found->at("type").get<std::string>();
  if ((type == "bool" && !value.is_boolean()) ||
      (type == "number" && (!value.is_number() || !std::isfinite(value.get<double>()))) ||
      (type == "string" && !value.is_string()))
    throw std::runtime_error("Invalid setting type");
  if (type == "enum" &&
      std::none_of((*found)["options"].begin(), (*found)["options"].end(),
                   [&](const auto &option) { return option.at("value") == value; }))
    throw std::runtime_error("Invalid setting option");
  if (found->contains("minimum") && (value < found->at("minimum") || value > found->at("maximum")))
    throw std::runtime_error("Setting outside permitted range");
  if (key == "ui/customAccent") {
    const auto color = value.get<std::string>();
    if (color.size() != 7 || color[0] != '#' ||
        color.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
      throw std::runtime_error("Use a six-digit hexadecimal accent colour");
  }
}
Json profile_runtime_values(const Json &values, const Json &global, bool freeze_defaults) {
  if (!values.is_object())
    throw std::runtime_error("Profile runtime overrides must be an object");
  const auto &definitions = profile_runtime_definitions();
  Json result = Json::object();
  if (freeze_defaults && values.empty())
    for (const auto &definition : definitions) {
      const auto key = definition.at("key").get<std::string>();
      result[key] = global.value(key, definition.at("default"));
    }
  for (auto field = values.begin(); field != values.end(); ++field) {
    if (std::none_of(definitions.begin(), definitions.end(),
                     [&](const auto &definition) { return definition.at("key") == field.key(); }))
      throw std::runtime_error("Unsupported profile runtime override: " + field.key());
    validate_setting(field.key(), field.value());
    result[field.key()] = field.value();
  }
  return result;
}
Json effective_profile_settings(const Json &profile, const Json &global) {
  auto settings = global;
  if (profile.value("isolatedSettings", false)) {
    const auto values =
        profile_runtime_values(profile.value("runtimeOverrides", Json::object()), global);
    for (auto field = values.begin(); field != values.end(); ++field)
      settings[field.key()] = field.value();
  }
  return settings;
}
void normalize_profile_preferences(Json &profile, const Json &global) {
  const auto lower = [](std::string value) {
    for (auto &c : value)
      if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
    return value;
  };
  auto region = profile.value("region", "Auto (Global)");
  if (region == "Europe")
    region = "PAL";
  else if (region == "North America")
    region = "NTSC-U";
  else if (region == "Japan" || region == "Asia")
    region = "NTSC-J";
  auto selected = std::string("Auto (Global)");
  for (const auto &option : profile_definitions().at("regions"))
    if (lower(option.get<std::string>()) == lower(region))
      selected = option.get<std::string>();
  profile["region"] = selected;
  auto page = profile.value("startupPage", "Launcher default");
  selected = "Launcher default";
  for (const auto &option : profile_definitions().at("startupPages"))
    if (lower(option.get<std::string>()) == lower(page))
      selected = option.get<std::string>();
  profile["startupPage"] = selected;
  profile["runtimeOverrides"] =
      profile.value("isolatedSettings", false)
          ? profile_runtime_values(profile.value("runtimeOverrides", Json::object()), global, true)
          : Json::object();
}
} // namespace xenon::launcher_next
