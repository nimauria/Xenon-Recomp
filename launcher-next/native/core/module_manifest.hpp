#pragma once
#include "state_file.hpp"
#include <filesystem>
namespace xenon::launcher_next {
std::string module_identity(const Json &manifest);
Json module_runtime_apis(const Json &manifest);
// Static declarations are checked without loading executable code. Binary ABI remains unverified.
Json module_compatibility(const Json &manifest, const std::filesystem::path &folder);
} // namespace xenon::launcher_next
