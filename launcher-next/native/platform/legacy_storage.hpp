#pragma once
#include "core/state_file.hpp"
#include <optional>

namespace xenon::launcher_next {
struct LegacyStorage {
  std::filesystem::path app, library, profiles, modules, games, saves, screenshots;
  Json values = Json::object();
  bool registry = false;
  bool mac_preferences = false;
  std::optional<Json> value(const std::string &key) const;
};
// Read-only. A supplied folder is a copied legacy data tree with optional flat settings.json.
LegacyStorage legacy_storage(const std::filesystem::path &folder = {});
Json read_legacy_ini(const std::filesystem::path &path);
std::string legacy_macos_key(const std::string &key);
std::filesystem::path legacy_local_file(const std::string &value);
} // namespace xenon::launcher_next
