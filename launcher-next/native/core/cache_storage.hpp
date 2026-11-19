#pragma once
#include "state_file.hpp"
#include <filesystem>
namespace xenon::launcher_next {
Json cache_location(const std::filesystem::path &storage, const Json &state);
std::filesystem::path cache_root(const std::filesystem::path &storage, const Json &location);
void check_cache_location(const std::filesystem::path &storage, const Json &location,
                          bool create = false);
bool cache_relocation_job(const Json &job);
} // namespace xenon::launcher_next
