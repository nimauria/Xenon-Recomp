#pragma once
#include <filesystem>
#include <map>
#include <string>
namespace xenon::launcher_next {
// Extraction is restricted to a fresh staging directory owned by the caller.
void extract_package(const std::filesystem::path &archive,
                     const std::filesystem::path &destination);
void write_zip(const std::filesystem::path &destination,
               const std::map<std::string, std::string> &files);
} // namespace xenon::launcher_next
