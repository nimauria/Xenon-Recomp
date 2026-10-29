#pragma once
#include "state_file.hpp"
namespace xenon::launcher_next {
void normalize_game_activity(Json &game);
bool apply_session_activity(Json &state, const Json &report);
Json game_activity_summary(const Json &state);
} // namespace xenon::launcher_next
