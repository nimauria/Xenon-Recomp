#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
namespace xenon::launcher_next {
using Json = nlohmann::json;
std::string make_id();
Json read_document(const std::filesystem::path &path, std::uintmax_t limit = 16 * 1024 * 1024);
void write_document(const std::filesystem::path &path, const Json &value);
} // namespace xenon::launcher_next
