#pragma once
#include "state_file.hpp"
namespace xenon::launcher_next {
bool same_media_source(const std::string &left, const std::string &right);
Json make_game_media(const std::string &path, const Json &metadata = Json::object());
void normalize_game_media(Json &game);
void validate_game_media(const Json &game);
Json &game_media(Json &game, const std::string &id);
void select_game_media(Json &game, const std::string &id);
void sync_game_media(Json &game);
} // namespace xenon::launcher_next
