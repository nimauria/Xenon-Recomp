#include "module_presentation.hpp"
#include <algorithm>
#include <cmath>
namespace xenon::launcher_next {
namespace {
std::string text(const Json &object, const char *key) {
  if (!object.is_object() || !object.contains(key) || !object.at(key).is_string())
    return {};
  return object.at(key).get<std::string>();
}
} // namespace
const Json *presentation_catalog_entry(const Json &state, const std::string &module_id) {
  if (state.contains("catalog"))
    for (const auto &entry : state.at("catalog").at("entries"))
      if (entry.at("id") == module_id)
        return &entry;
  return nullptr;
}
void apply_module_presentation(Json &game, const Json &module, const Json *catalog_entry,
                               std::int64_t revision, bool refresh_title) {
  auto metadata = module.at("manifest").value("launcher", Json::object());
  if (!metadata.is_object())
    metadata = Json::object();
  const auto catalog =
      catalog_entry ? catalog_entry->value("launcher", Json::object()) : Json::object();
  for (const auto *key : {"gameTitle", "description"})
    if (!text(catalog, key).empty())
      metadata[key] = catalog.at(key);
  for (const auto *kind : {"tile", "hero"}) {
    const auto prefix = std::string(kind), field = prefix + "Art";
    const auto cached = text(catalog, ("_" + prefix + "File").c_str());
    // Catalogue image paths are not file paths or remote URLs in the renderer. Only the bounded
    // cache resource is used. If a fetch failed, the installed module artwork remains the fallback.
    if (!cached.empty() && cached.find_first_of("/\\:") == std::string::npos && cached != "." &&
        cached != "..") {
      game[field] = "xenon://app/catalog-" + prefix + "/" + module.at("id").get<std::string>() +
                    "?v=" + text(catalog, ("_" + prefix + "Version").c_str());
      for (const auto *axis : {"X", "Y"}) {
        const auto focal = field + "Focal" + axis;
        metadata.erase(focal);
        if (catalog.contains(focal))
          metadata[focal] = catalog.at(focal);
      }
    } else if (!text(metadata, field.c_str()).empty()) {
      game[field] = "xenon://app/game-" + prefix + "/" + game.at("id").get<std::string>() +
                    "?v=" + std::to_string(revision);
    } else {
      game.erase(field);
    }
  }
  if (refresh_title) {
    auto title = text(metadata, "gameTitle");
    const auto first = title.find_first_not_of(" \t\r\n");
    if (first != std::string::npos)
      game["title"] = title.substr(first, title.find_last_not_of(" \t\r\n") - first + 1);
  }
  if (metadata.contains("description") && metadata.at("description").is_string())
    game["description"] = metadata.at("description");
  for (const auto *key : {"tileArtFocalX", "tileArtFocalY", "heroArtFocalX", "heroArtFocalY"}) {
    game.erase(key);
    if (metadata.contains(key) && metadata.at(key).is_number() &&
        std::isfinite(metadata.at(key).get<double>()))
      game[key] = std::clamp(metadata.at(key).get<double>(), 0., 1.);
  }
}
} // namespace xenon::launcher_next
