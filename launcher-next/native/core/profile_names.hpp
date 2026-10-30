#pragma once
#include <string>
#include <string_view>
namespace xenon::launcher_next {
std::string trim_profile_name(std::string_view name);
std::string profile_name_key(std::string_view name);
} // namespace xenon::launcher_next
