#pragma once
#include "state_file.hpp"
namespace xenon::launcher_next {
const Json *presentation_catalog_entry(const Json &state, const std::string &module_id);
void apply_module_presentation(Json &game, const Json &module, const Json *catalog_entry,
                               std::int64_t revision, bool refresh_title = false);
} // namespace xenon::launcher_next
