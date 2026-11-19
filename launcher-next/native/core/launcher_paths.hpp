#pragma once
#include "state_file.hpp"
#include <filesystem>
namespace xenon::launcher_next {
bool launcher_destination_key(const std::string &key);
void validate_launcher_path(const Json &value);
std::filesystem::path launcher_destination(const std::filesystem::path &storage,
                                           const Json &settings, const std::string &key);
Json launcher_locations(const std::filesystem::path &storage, const Json &settings);
std::filesystem::path launcher_location(const std::filesystem::path &storage, const Json &settings,
                                        const std::string &id);
Json launcher_build_info();
} // namespace xenon::launcher_next
