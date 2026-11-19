#include "cache_storage.hpp"
#include "archive.hpp"
#include "file_transfer.hpp"
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
std::string text(const fs::path &path) {
  const auto u = path.u8string();
  return {u.begin(), u.end()};
}
} // namespace
Json cache_location(const fs::path &storage, const Json &state) {
  if (state.contains("cache") && state.at("cache").value("version", 0) == 1)
    return state.at("cache");
  return {{"version", 1}, {"root", text(storage)}, {"ownerId", ""}, {"configuredPath", ""}};
}
fs::path cache_root(const fs::path &storage, const Json &location) {
  const auto root = fs::u8path(location.at("root").get<std::string>());
  const auto id = location.value("ownerId", "");
  if (!root.is_absolute() || root != root.lexically_normal())
    throw std::runtime_error("Invalid recorded cache location");
  if (root == storage && id.empty())
    return root;
  if (id.empty() || id.size() > 100 ||
      id.find_first_not_of("0123456789abcdef-") != std::string::npos ||
      root.filename() != fs::u8path("xenon-cache-" + id))
    throw std::runtime_error("Invalid managed cache identity");
  return root;
}
void check_cache_location(const fs::path &storage, const Json &location, bool create) {
  const auto root = cache_root(storage, location);
  if (root == storage) {
    if (!fs::is_directory(root) || redirected_path(root) || fs::weakly_canonical(root) != root)
      throw std::runtime_error("Launcher cache storage was redirected or is unavailable");
    return;
  }
  const auto parent = root.parent_path();
  if (!fs::is_directory(parent) || redirected_path(parent) ||
      fs::weakly_canonical(parent) != parent)
    throw std::runtime_error(
        "The cache destination folder was moved, redirected or is unavailable");
  if (!fs::exists(root)) {
    if (!create)
      return;
    if (!fs::create_directory(root))
      throw std::runtime_error("Could not create the managed cache folder");
  }
  if (redirected_path(root) || fs::weakly_canonical(root) != root || !fs::is_directory(root))
    throw std::runtime_error("The managed cache folder was redirected");
  const auto marker = root / "owner.json";
  const Json expected = {{"owner", "xenon-launcher-next-cache"}, {"id", location.at("ownerId")}};
  if (create && !fs::exists(marker) && fs::is_empty(root))
    write_document(marker, expected);
  if (!fs::is_regular_file(marker) || redirected_path(marker) ||
      read_document(marker, 4096) != expected)
    throw std::runtime_error("Cache ownership changed; existing files were kept");
}
bool cache_relocation_job(const Json &job) {
  return job.value("kind", "") == "service.operation" &&
         job.value("_command", "") == "cache.relocate";
}
} // namespace xenon::launcher_next
