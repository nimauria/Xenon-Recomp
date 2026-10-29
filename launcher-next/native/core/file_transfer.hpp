#pragma once
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <vector>
namespace xenon::launcher_next {
// All recursive operations reject links, junctions and other redirected entries.
bool redirected_path(const std::filesystem::path &path);
void check_transfer_tree(const std::filesystem::path &path);
nlohmann::json
copy_library_content(const std::filesystem::path &source, const std::filesystem::path &destination,
                     const std::function<void()> &check,
                     const std::function<void(std::uint64_t, std::uint64_t)> &progress);
// Returns false when new/changed/unremovable source files were retained.
bool remove_transferred_source(const std::filesystem::path &source,
                               const std::filesystem::path &destination,
                               const nlohmann::json &manifest, const std::function<void()> &check);
void verify_library_content(const std::filesystem::path &destination,
                            const nlohmann::json &manifest, const std::function<void()> &check);
nlohmann::json
copy_library_sources(const std::vector<std::filesystem::path> &sources,
                     const std::filesystem::path &destination, const std::function<void()> &check,
                     const std::function<void(std::uint64_t, std::uint64_t)> &progress);
} // namespace xenon::launcher_next
