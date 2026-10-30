#include "launcher_service.hpp"
#include "core/cache_storage.hpp"
#include "core/file_transfer.hpp"
#include "core/launcher_paths.hpp"
#include "core/archive.hpp"
#include <algorithm>
#include <chrono>
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
bool active(const Json &job) {
  const auto s = job.value("state", "");
  return s == "queued" || s == "running" || s == "cancelling" || s == "committing" ||
         job.value("_cleanup", false);
}
bool within(const fs::path &path, const fs::path &root) { return path_within(path, root); }
Json download_location(const fs::path &storage, const Json &job) {
  return job.value("_cacheLocation", cache_location(storage, Json::object()));
}
} // namespace
Json LauncherService::prepare_cache_relocation(const Json &next, const Json &data) {
  for (const auto &job : next.at("jobs"))
    if (active(job))
      throw std::runtime_error(
          "Wait for current file operations to finish before changing the cache location");
  const auto configured = data.value("path", "");
  validate_launcher_path(configured);
  const auto source = cache_location(storage_, next);
  const auto source_root = cache_root(storage_, source);
  const auto parent = configured.empty() ? storage_ : fs::u8path(configured).lexically_normal();
  if (!fs::is_directory(parent) || redirected_path(parent) ||
      fs::weakly_canonical(parent) != parent)
    throw std::runtime_error("Choose an existing cache folder without links or junctions");
  if (parent != storage_ && (within(parent, storage_) || within(storage_, parent)))
    throw std::runtime_error(
        "Choose a cache folder separate from launcher data, or use the default");
  for (const auto &previous : next.value("cacheHistory", Json::array()))
    if (parent != storage_ && within(parent, cache_root(storage_, previous)))
      throw std::runtime_error("Choose a folder outside retained cache copies");
  if (parent != storage_ && within(parent, source_root))
    throw std::runtime_error("Choose a folder outside the current managed cache");
  const bool noop = configured == source.value("configuredPath", "");
  const auto owner = make_id();
  const Json target = noop ? source
                           : Json({{"version", 1},
                                   {"root", path_string(parent / ("xenon-cache-" + owner))},
                                   {"ownerId", owner},
                                   {"configuredPath", configured}});
  return {{"source", source}, {"target", target}, {"noop", noop}};
}
void LauncherService::cleanup_cache_relocation(const Json &job) {
  const auto target = job.at("_data").at("target");
  const auto root = cache_root(storage_, target);
  if (root == storage_ || !fs::exists(root))
    return;
  {
    std::lock_guard lock(mutex_);
    if (cache_root(storage_, cache_location(storage_, state_)) == root)
      return;
    for (const auto &old : state_.value("cacheHistory", Json::array()))
      if (cache_root(storage_, old) == root)
        return;
    const auto protect = [&](const std::string &value) {
      if (!value.empty() && within(fs::absolute(fs::u8path(value)), root))
        throw std::runtime_error(
            "Temporary cache output is referenced by registered data; its files were kept");
    };
    for (const auto &module : state_["modules"]) {
      protect(module.value("path", ""));
      for (const auto &previous : module.value("previousVersions", Json::array()))
        protect(previous.value("path", ""));
    }
    for (const auto &game : state_["games"]) {
      protect(game.value("contentPath", ""));
      for (const auto &media : game.value("media", Json::array()))
        protect(media.value("sourcePath", ""));
      for (const auto &dlc : game.value("dlc", Json::array()))
        protect(dlc.value("path", ""));
      for (const auto &copy : game.value("managedCopies", Json::array())) {
        protect(copy.value("root", ""));
        protect(copy.value("originalPath", ""));
      }
    }
    for (const auto &profile : state_["profiles"])
      for (const auto *key : {"gamePath", "savePath", "screenshotPath"})
        protect(profile.value(key, ""));
    for (auto setting = state_["settings"].begin(); setting != state_["settings"].end(); ++setting)
      if (setting.key().starts_with("paths/") && setting.value().is_string())
        protect(setting.value().get<std::string>());
  }
  // Persistence may have committed before an observer/process failure. Never remove its active
  // root.
  if (fs::is_regular_file(storage_ / "state.json")) {
    const auto saved = read_document(storage_ / "state.json");
    if (cache_root(storage_, cache_location(storage_, saved)) == root)
      return;
  }
  if (redirected_path(root) || fs::weakly_canonical(root) != root ||
      fs::weakly_canonical(root.parent_path()) != root.parent_path())
    throw std::runtime_error("Cache cleanup location was redirected; files were kept");
  if (fs::is_empty(root)) {
    fs::remove(root);
    return;
  }
  check_cache_location(storage_, target);
  check_transfer_tree(root);
  fs::remove_all(root);
}
void LauncherService::run_cache_relocation(const Json &job,
                                           const std::shared_ptr<std::atomic_bool> &cancel) {
  const auto id = job.at("id").get<std::string>();
  const auto &options = job.at("_data");
  const auto source = options.at("source"), target = options.at("target");
  const auto source_root = cache_root(storage_, source), target_root = cache_root(storage_, target);
  const auto check = [&] {
    if (cancel->load())
      throw OperationCancelled();
  };
  if (!options.at("noop").get<bool>()) {
    check_cache_location(storage_, source, true);
    check_cache_location(storage_, target, true);
    auto last = std::chrono::steady_clock::time_point{};
    for (const auto *component : {"catalog-art", "downloads"}) {
      check();
      const auto original = source_root / component;
      if (!fs::exists(original))
        continue;
      check_transfer_tree(original);
      const auto receipt = copy_library_content(
          original, target_root / component, check, [&](auto done, auto total) {
            const auto current = std::chrono::steady_clock::now();
            if (current - last < std::chrono::milliseconds(200))
              return;
            update_job(
                id, {{"phase", std::string("Copying cache ") + component},
                     {"progress", total ? std::min(90, static_cast<int>(90. * done / total)) : 10},
                     {"bytesDone", done},
                     {"bytesTotal", total}});
            last = current;
          });
      verify_library_content(target_root / component, receipt, check);
    }
    check_cache_location(storage_, target);
    check_transfer_tree(target_root);
  }
  if (install_observer_)
    install_observer_(id, "Cache relocation ready");
  check();
  complete_job(id, [&](Json &next) {
    if (cache_location(storage_, next) != source)
      throw std::runtime_error(
          "Cache location changed during copying; the previous cache was kept");
    check();
    if (!options.at("noop").get<bool>()) {
      check_cache_location(storage_, target);
      if (!next.contains("cacheHistory"))
        next["cacheHistory"] = Json::array();
      next["cacheHistory"].push_back(source);
      // The package byte content and digest do not change; only references to copied files move.
      const auto rewrite = [&](Json &value) {
        if (value.contains("_packagePath")) {
          const auto path = fs::u8path(value.at("_packagePath").get<std::string>());
          const auto relative = path.lexically_relative(source_root / "downloads");
          if (!relative.empty() && *relative.begin() != "..")
            value["_packagePath"] = path_string(target_root / "downloads" / relative);
        }
        if (value.contains("_downloadId"))
          value["_cacheLocation"] = target;
      };
      for (auto &value : next["moduleUpdates"])
        rewrite(value);
      rewrite(next["launcherUpdate"]);
      for (auto &operation : next["jobs"])
        if ((operation.value("kind", "") == "module.download" ||
             operation.value("kind", "") == "launcher.download") &&
            download_location(storage_, operation) == source)
          operation["_cacheLocation"] = target;
      next["cache"] = target;
    }
    if (target.value("configuredPath", "").empty())
      next["settings"].erase("paths/cache");
    else
      next["settings"]["paths/cache"] = target.at("configuredPath");
    notify(next, "Cache location updated",
           "Managed artwork and downloads use the selected location. Previous copies were kept.");
  });
}
} // namespace xenon::launcher_next
