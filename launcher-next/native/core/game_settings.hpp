#pragma once
#include "state_file.hpp"
namespace xenon::launcher_next {
Json game_runtime_values(const Json &values);
Json effective_game_settings(const Json &game, const Json &profile, const Json &global);
Json game_module_values(const Json &schema, const Json &values);
Json effective_game_module_settings(const Json &game, const Json &module);
} // namespace xenon::launcher_next
