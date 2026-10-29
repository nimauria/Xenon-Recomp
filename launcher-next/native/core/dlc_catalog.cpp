#include "dlc_catalog.hpp"
#include <regex>
#include <set>
namespace xenon::launcher_next {
namespace {
std::string first(const Json &value, std::initializer_list<const char *> keys) {
  for (const auto key : keys)
    if (value.contains(key) && value.at(key).is_string()) {
      auto text = value.at(key).get<std::string>();
      const auto begin = text.find_first_not_of(" \t\r\n");
      if (begin != std::string::npos)
        return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
    }
  return {};
}
} // namespace
Json normalize_dlc_definitions(const Json &manifest) {
  Json raw = Json::array();
  const auto take = [&](const Json &object, const char *key) {
    if (raw.empty() && object.is_object() && object.contains(key) && object.at(key).is_array())
      raw = object.at(key);
  };
  for (const auto key : {"dlc", "addOns", "addons"})
    take(manifest, key);
  for (const auto key : {"launcher", "content"})
    if (manifest.contains(key))
      for (const auto field : {"dlc", "addOns"})
        take(manifest.at(key), field);
  if (raw.size() > 2048)
    throw std::runtime_error("Add-on catalogue exceeds 2,048 entries");
  Json result = Json::array();
  std::set<std::string> ids;
  static const std::regex valid("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$");
  std::size_t index = 0;
  for (const auto &source : raw) {
    ++index;
    if (!source.is_object())
      continue;
    auto name = first(source, {"name", "title", "displayName"}),
         id = first(source, {"id", "dlcId", "contentId", "content_id"});
    if (name.empty() && id.empty())
      continue;
    if (name.empty())
      name = id;
    if (!std::regex_match(id, valid)) {
      id.clear();
      bool separator = false;
      for (unsigned char c : name) {
        if (c >= 'A' && c <= 'Z')
          c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
          if (separator && !id.empty())
            id += '-';
          separator = false;
          id += char(c);
        } else
          separator = true;
      }
      while (!id.empty() && id.front() == '-')
        id.erase(id.begin());
      while (!id.empty() && id.back() == '-')
        id.pop_back();
      if (id.empty())
        id = "dlc-" + std::to_string(index);
      if (id.size() > 120)
        id.resize(120);
    }
    auto unique = id;
    for (unsigned suffix = 2; ids.contains(unique); ++suffix)
      unique = id + "-" + std::to_string(suffix);
    ids.insert(unique);
    auto content_ids = source.value("contentIds", source.value("content_ids", Json::array()));
    if (!content_ids.is_array())
      content_ids = Json::array();
    Json checked_ids = Json::array();
    for (const auto &value : content_ids)
      if (value.is_string() && value.get<std::string>().size() <= 128 && checked_ids.size() < 128)
        checked_ids.push_back(value);
    result.push_back({{"dlcId", unique},
                      {"name", name},
                      {"description", first(source, {"description", "summary"})},
                      {"version", first(source, {"version"})},
                      {"contentIds", checked_ids},
                      {"category", first(source, {"category"})},
                      {"group", first(source, {"group"})}});
  }
  return result;
}
Json launcher_dlc_catalog(const Json &state) {
  Json result = Json::object();
  for (const auto &module : state.at("modules"))
    try {
      result[module.at("id").get<std::string>()] = normalize_dlc_definitions(module.at("manifest"));
    } catch (const std::exception &) {
      result[module.at("id").get<std::string>()] = Json::array();
    }
  if (state.contains("catalog"))
    for (const auto &entry : state.at("catalog").at("entries")) {
      const auto definitions = normalize_dlc_definitions(entry);
      if (!definitions.empty())
        result[entry.at("id").get<std::string>()] = definitions;
    }
  return result;
}
Json selected_dlc_definition(const Json &state, const std::string &game_id,
                             const std::string &dlc_id) {
  std::string module_id;
  for (const auto &game : state.at("games"))
    if (game.at("id") == game_id)
      module_id = game.value("moduleId", "");
  const auto catalog = launcher_dlc_catalog(state);
  if (catalog.contains(module_id))
    for (const auto &definition : catalog.at(module_id))
      if (definition.at("dlcId") == dlc_id)
        return definition;
  throw std::runtime_error("This add-on is no longer declared for the game's selected module");
}
} // namespace xenon::launcher_next
