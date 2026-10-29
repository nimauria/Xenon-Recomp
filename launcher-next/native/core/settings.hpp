#pragma once
#include "state_file.hpp"
namespace xenon::launcher_next {
void validate_setting(const std::string &key, const Json &value);
Json profile_runtime_values(const Json &values, const Json &global, bool freeze_defaults = false);
Json effective_profile_settings(const Json &profile, const Json &global);
void normalize_profile_preferences(Json &profile, const Json &global);
} // namespace xenon::launcher_next
