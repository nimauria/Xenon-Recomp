#include "launcher_service.hpp"
#include "core/game_media.hpp"
#include "core/file_transfer.hpp"
#include <algorithm>
namespace xenon::launcher_next {
namespace fs = std::filesystem;
void LauncherService::execute_media(Json &next, const std::string &command, const Json &data) {
  const auto game_id = data.at("id").get<std::string>();
  if (command != "library.media.check")
    require_library_idle(next, game_id);
  auto found = std::find_if(next["games"].begin(), next["games"].end(),
                            [&](const auto &game) { return game.at("id") == game_id; });
  if (found == next["games"].end())
    throw std::runtime_error("This game no longer exists");
  auto &game = *found;
  normalize_game_media(game);
  if (command == "library.media.add") {
    const auto &paths = data.at("paths");
    const auto mode = data.value("mode", "keep");
    if (!paths.is_array() || paths.empty() || paths.size() > 128 ||
        (mode != "keep" && mode != "move"))
      throw std::runtime_error("Choose one to 128 game sources and a valid storage mode");
    Json moves = Json::array();
    for (const auto &value : paths) {
      const auto requested = fs::absolute(fs::u8path(value.get<std::string>())).lexically_normal();
      if (mode == "move" && redirected_path(requested))
        throw std::runtime_error(
            "Choose original media rather than a filesystem link before moving it");
      const auto source = fs::canonical(requested);
      auto ext = path_string(source.extension());
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      if (!fs::is_directory(source) && ext != ".xex" && ext != ".iso" && ext != ".xgd" &&
          ext != ".dvd")
        throw std::runtime_error("Choose a game folder, XEX or disc image");
      const auto path = path_string(source);
      if (std::any_of(game["media"].begin(), game["media"].end(), [&](const auto &medium) {
            return same_media_source(medium.at("sourcePath"), path);
          }))
        continue;
      if (game["media"].size() >= 128)
        throw std::runtime_error("A game can contain at most 128 media references");
      auto medium = make_game_media(path);
      if (game["media"].empty())
        medium["isPrimary"] = true;
      game["media"].push_back(medium);
      if (mode == "move")
        moves.push_back({{"id", game_id},
                         {"mediaId", medium.at("id")},
                         {"path", path},
                         {"type", "game"},
                         {"mode", "move"}});
    }
    sync_game_media(game);
    for (const auto &move : moves)
      enqueue_library(next, "library.transfer", move);
    notify(next, "Game media added",
           "Sources grouped under this game. Content identity is awaiting the Xenon adapter.");
  } else if (command == "library.media.check") {
    auto &medium = game_media(game, data.at("mediaId"));
    std::error_code error;
    const bool exists = fs::exists(fs::u8path(medium.at("sourcePath").get<std::string>()), error);
    if (error)
      throw std::runtime_error("Could not inspect this media source: " + error.message());
    medium["missing"] = !exists;
    notify(next, exists ? "Media source exists" : "Media source missing",
           exists ? "The registered source exists. Integrity and compatibility require the Xenon "
                    "content adapter."
                  : "The source path is missing. Its reference and metadata were kept.",
           exists ? "info" : "warning");
  } else if (command == "library.media.select") {
    select_game_media(game, data.at("mediaId"));
  } else if (command == "library.media.update") {
    auto &medium = game_media(game, data.at("mediaId"));
    for (const auto *key : {"name", "discNumber", "discCount", "region"})
      if (data.contains(key))
        medium[key] = data.at(key);
    if (data.contains("contentMediaId"))
      medium["mediaId"] = data.at("contentMediaId");
    medium["identityVerified"] = false;
    validate_game_media(game);
  } else if (command == "library.media.remove") {
    const auto media_id = data.at("mediaId").get<std::string>();
    (void)game_media(game, media_id);
    if (game["media"].size() == 1)
      throw std::runtime_error("Keep at least one source, or remove the entire library reference");
    for (const auto &copy : game.value("managedCopies", Json::array()))
      if (copy.value("_type", "") == "game" && copy.value("entryId", "") == media_id)
        throw std::runtime_error("Delete this disc's managed files before removing its reference");
    auto &media = game["media"];
    media.erase(std::remove_if(media.begin(), media.end(),
                               [&](const auto &value) { return value.at("id") == media_id; }),
                media.end());
    if (std::none_of(media.begin(), media.end(),
                     [](const auto &value) { return value.value("isPrimary", false); }))
      select_game_media(game, media.front().at("id"));
    sync_game_media(game);
  } else
    throw std::runtime_error("Unknown game media operation");
}
} // namespace xenon::launcher_next
