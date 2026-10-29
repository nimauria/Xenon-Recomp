#include "game_media.hpp"
#include <algorithm>
#include <chrono>
#include <set>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
std::string text(const fs::path &path) {
  const auto u = path.u8string();
  return {u.begin(), u.end()};
}
int disc_value(const Json &value, const char *key) {
  if (!value.contains(key) || !value.at(key).is_number_integer())
    return 0;
  const auto number = value.at(key).get<std::int64_t>();
  return number >= 0 && number <= 99 ? static_cast<int>(number) : 0;
}
std::string field(const Json &value, const char *key, const std::string &fallback = "") {
  return value.contains(key) && value.at(key).is_string() ? value.at(key).get<std::string>()
                                                          : fallback;
}
} // namespace
bool same_media_source(const std::string &left, const std::string &right) {
  if (left.empty() || right.empty())
    return false;
  const auto a = fs::u8path(left).lexically_normal(), b = fs::u8path(right).lexically_normal();
#ifdef _WIN32
  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
  return a == b;
#endif
}
Json make_game_media(const std::string &path, const Json &metadata) {
  const auto source = fs::u8path(path);
  const auto count = disc_value(metadata, "discCount"), number = disc_value(metadata, "discNumber");
  auto mode = field(metadata, "importMode", "kept");
  if (mode != "kept" && mode != "copied" && mode != "moved")
    mode = "kept";
  return {{"id", make_id()},
          {"name", field(metadata, "name", text(source.stem()))},
          {"sourcePath", path},
          {"sourceType", field(metadata, "sourceType",
                               source.has_extension() ? text(source.extension()) : "folder")},
          {"mediaId", field(metadata, "mediaId")},
          {"discNumber", count > 0 && number > count ? 0 : number},
          {"discCount", count},
          {"region", field(metadata, "region")},
          {"importMode", mode},
          {"isPrimary", false},
          {"addedAt", field(metadata, "addedAt",
                            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::system_clock::now().time_since_epoch())
                                               .count()))},
          {"identityVerified", false}};
}
Json &game_media(Json &game, const std::string &id) {
  for (auto &medium : game["media"])
    if (medium.value("id", "") == id)
      return medium;
  throw std::runtime_error("The selected media reference no longer exists");
}
void sync_game_media(Json &game) {
  for (const auto &medium : game.at("media"))
    if (medium.value("isPrimary", false)) {
      game["contentPath"] = medium.at("sourcePath");
      return;
    }
  if (!game.at("media").empty()) {
    game["media"][0]["isPrimary"] = true;
    game["contentPath"] = game["media"][0].at("sourcePath");
  }
}
void select_game_media(Json &game, const std::string &id) {
  (void)game_media(game, id);
  for (auto &medium : game["media"])
    medium["isPrimary"] = medium.at("id") == id;
  sync_game_media(game);
  game["verified"] = false;
  game.erase("contentReport");
}
void normalize_game_media(Json &game) {
  if (game.value("mediaModelVersion", 0) == 1)
    return;
  const auto path = game.value("contentPath", "");
  Json source = game.value("media", Json::array());
  if (source.empty() && game.contains("legacyMetadata"))
    source = game.at("legacyMetadata").value("media", Json::array());
  if (!source.is_array() || source.size() > 128)
    throw std::runtime_error("Invalid saved media collection");
  Json media = Json::array();
  for (const auto &record : source)
    if (record.is_object()) {
      const auto source_path = field(record, "sourcePath");
      if (source_path.empty())
        continue;
      if (std::any_of(media.begin(), media.end(), [&](const auto &row) {
            return same_media_source(row.at("sourcePath"), source_path);
          }))
        continue;
      auto medium = make_game_media(source_path, record);
      medium["legacyMetadata"] = record;
      media.push_back(medium);
    }
  if (!path.empty() && std::none_of(media.begin(), media.end(), [&](const auto &row) {
        return same_media_source(row.at("sourcePath"), path);
      }))
    media.push_back(make_game_media(path));
  if (media.size() > 128)
    throw std::runtime_error("A game can contain at most 128 media references");
  std::string primary;
  for (auto &medium : media) {
    if (same_media_source(medium.at("sourcePath"), path))
      primary = medium.at("id");
    for (const auto &copy : game.value("managedCopies", Json::array()))
      if (copy.value("_type", "") == "game") {
        const auto root = fs::u8path(copy.value("root", ""));
        const auto relative =
            fs::u8path(medium.at("sourcePath").get<std::string>()).lexically_relative(root);
        if (!root.empty() && !relative.empty() && *relative.begin() != "..") {
          const auto old_id = medium.at("id").get<std::string>();
          medium["id"] = copy.at("entryId");
          medium["importMode"] = copy.value("originalRetained", true) ? "copied" : "moved";
          if (primary == old_id)
            primary = medium.at("id");
        }
      }
  }
  game["media"] = media;
  game["mediaModelVersion"] = 1;
  if (!media.empty()) {
    const auto selected = primary.empty() ? media.front().at("id").get<std::string>() : primary;
    for (auto &medium : game["media"])
      medium["isPrimary"] = medium.at("id") == selected;
    sync_game_media(game);
  }
}
void validate_game_media(const Json &game) {
  if (game.value("mediaModelVersion", 0) != 1)
    return;
  const auto &media = game.at("media");
  if (!media.is_array() || media.empty() || media.size() > 128)
    throw std::runtime_error("A game requires one to 128 media references");
  std::set<std::string> ids;
  unsigned primary = 0;
  for (const auto &medium : media) {
    if (!medium.at("id").is_string() || medium.at("id").get<std::string>().empty() ||
        !ids.insert(medium.at("id").get<std::string>()).second ||
        !medium.at("sourcePath").is_string() || medium.at("sourcePath").get<std::string>().empty())
      throw std::runtime_error("Invalid or duplicate media reference");
    for (const auto *key : {"discNumber", "discCount"})
      if (!medium.at(key).is_number_integer() || medium.at(key).get<std::int64_t>() < 0 ||
          medium.at(key).get<std::int64_t>() > 99)
        throw std::runtime_error("Disc numbers must be between zero and 99");
    if (medium.at("discCount").get<int>() > 0 &&
        medium.at("discNumber").get<int>() > medium.at("discCount").get<int>())
      throw std::runtime_error("Disc number exceeds the disc count");
    for (const auto *key : {"name", "mediaId", "region", "sourceType", "importMode"})
      if (!medium.at(key).is_string() || medium.at(key).get<std::string>().size() > 4096)
        throw std::runtime_error("Invalid media metadata");
    if (medium.at("importMode") != "kept" && medium.at("importMode") != "copied" &&
        medium.at("importMode") != "moved")
      throw std::runtime_error("Invalid media storage mode");
    if (!medium.at("isPrimary").is_boolean())
      throw std::runtime_error("Invalid primary media flag");
    if (medium.at("isPrimary").get<bool>()) {
      ++primary;
      if (game.at("contentPath") != medium.at("sourcePath"))
        throw std::runtime_error("Primary media does not match the game's launch source");
    }
  }
  if (primary != 1)
    throw std::runtime_error("Select exactly one primary disc or content source");
}
} // namespace xenon::launcher_next
