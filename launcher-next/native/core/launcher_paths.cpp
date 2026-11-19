#include "launcher_paths.hpp"
#include <array>
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
struct Destination {
  const char *key, *id, *label, *folder;
};
constexpr std::array<Destination, 4> destinations{
    {{"paths/games", "games", "Games", "games"},
     {"paths/saves", "saves", "Saves", "saves"},
     {"paths/modules", "modules", "Modules", "modules"},
     {"paths/screenshots", "screenshots", "Screenshots", "captures"}}};
std::string text(const fs::path &path) {
  const auto u = path.u8string();
  return {u.begin(), u.end()};
}
bool valid(const Json &value) {
  if (!value.is_string())
    return false;
  const auto path = value.get<std::string>();
  return path.empty() ||
         (path.size() <= 32768 && path.find('\0') == std::string::npos &&
          path.find_first_of("\r\n") == std::string::npos && fs::u8path(path).is_absolute());
}
} // namespace
bool launcher_destination_key(const std::string &key) {
  for (const auto &entry : destinations)
    if (key == entry.key)
      return true;
  return false;
}
void validate_launcher_path(const Json &value) {
  if (!valid(value))
    throw std::runtime_error(
        "Choose an absolute folder path, or leave it blank to use the default");
}
fs::path launcher_destination(const fs::path &storage, const Json &settings,
                              const std::string &key) {
  for (const auto &entry : destinations)
    if (key == entry.key) {
      const auto configured = settings.value(key, Json(""));
      // Invalid old values are retained for review but never redirect a new operation to the cwd.
      if (valid(configured) && !configured.get<std::string>().empty())
        return fs::u8path(configured.get<std::string>()).lexically_normal();
      return storage / entry.folder;
    }
  throw std::runtime_error("Unknown launcher destination");
}
Json launcher_locations(const fs::path &storage, const Json &settings) {
  Json result = {{"destinations", Json::array()}, {"fixed", Json::array()}};
  for (const auto &entry : destinations) {
    const auto configured = settings.value(entry.key, Json(""));
    result["destinations"].push_back(
        {{"id", entry.id},
         {"key", entry.key},
         {"label", entry.label},
         {"defaultPath", text(storage / entry.folder)},
         {"configuredPath", configured.is_string() ? configured : Json(configured.dump())},
         {"effectivePath", text(launcher_destination(storage, settings, entry.key))},
         {"invalid", !valid(configured)}});
  }
  const auto fixed = [&](const char *id, const char *label, const fs::path &path,
                         const char *description, const char *legacy_key = "") {
    const auto legacy = settings.value(legacy_key, Json(""));
    result["fixed"].push_back(
        {{"id", id},
         {"label", label},
         {"path", text(path)},
         {"description", description},
         {"legacyKey", legacy_key},
         {"legacyConfiguredPath", legacy.is_string() ? legacy : Json(legacy.dump())}});
  };
  fixed("storage", "Launcher data", storage,
        "Change the launcher data directory at startup with --data-dir.");
  fixed("profiles", "Profiles and settings", storage / "state.json",
        "Stored together in the launcher data file. Individual profiles can be exported.",
        "paths/profiles");
  fixed("avatars", "Profile images", storage / "avatars",
        "Imported profile images are stored with launcher data.");
  fixed("artwork", "Artwork cache", storage / "catalog-art",
        "Cache relocation is not available in this launcher yet.", "paths/cache");
  fixed("downloads", "Downloaded packages", storage / "downloads",
        "Verified downloads remain in launcher data; installing uses the selected Modules "
        "destination.");
  return result;
}
fs::path launcher_location(const fs::path &storage, const Json &settings, const std::string &id) {
  for (const auto &entry : destinations)
    if (id == entry.id)
      return launcher_destination(storage, settings, entry.key);
  if (id == "storage" || id == "profiles")
    return storage;
  if (id == "avatars")
    return storage / "avatars";
  if (id == "artwork")
    return storage / "catalog-art";
  if (id == "downloads")
    return storage / "downloads";
  throw std::runtime_error("Unknown launcher location");
}
Json launcher_build_info() {
  Json result = {{"version", XENON_LAUNCHER_VERSION}, {"protocolVersion", 1}};
#ifdef _WIN32
  result["platform"] = "Windows";
#elif defined(__APPLE__)
  result["platform"] = "macOS";
#else
  result["platform"] = "Linux";
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
  result["architecture"] = "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
  result["architecture"] = "x64";
#elif defined(_M_IX86) || defined(__i386__)
  result["architecture"] = "x86";
#else
  result["architecture"] = "unknown";
#endif
  return result;
}
} // namespace xenon::launcher_next
