#pragma once
#include "state_file.hpp"
#include <atomic>
#include <memory>
namespace xenon::launcher_next {
Json legacy_snapshot(const std::filesystem::path &folder,
                     const std::shared_ptr<std::atomic_bool> &cancel);
Json legacy_plan(const Json &input, const Json &state, const std::string &id, bool replace_settings,
                 bool activate);
std::string migration_target_hash(const Json &state);
std::string migration_hash(const Json &value);
std::string migration_file_hash(const std::filesystem::path &file,
                                const std::shared_ptr<std::atomic_bool> &cancel);
} // namespace xenon::launcher_next
