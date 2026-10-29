#pragma once
#include "state_file.hpp"
namespace xenon::launcher_next {
Json normalize_dlc_definitions(const Json &manifest);
Json launcher_dlc_catalog(const Json &state);
Json selected_dlc_definition(const Json &state, const std::string &game_id,
                             const std::string &dlc_id);
} // namespace xenon::launcher_next
