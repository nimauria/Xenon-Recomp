#pragma once
#include "state_file.hpp"

namespace xenon::launcher_next {
// Normalize legacy manifest aliases once, before exposing controls to the renderer.
Json module_settings_schema(const Json &manifest);
void validate_module_setting(const Json &definition, const Json &value);
Json module_settings_values(const Json &schema, const Json &overrides);
} // namespace xenon::launcher_next
