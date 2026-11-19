#include "launcher_service.hpp"
#include "core/archive.hpp"
#include "core/file_transfer.hpp"
#include <algorithm>
#include <set>
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
bool active(const Json &job) {
  const auto status = job.value("state", "");
  return status == "queued" || status == "running" || status == "cancelling" ||
         status == "committing";
}
bool within(const fs::path &path, const fs::path &root) { return path_within(path, root); }
bool identity(const std::string &id) {
  return !id.empty() && id.size() <= 100 &&
         id.find_first_not_of("0123456789abcdef-") == std::string::npos;
}
Json marker(const std::string &id) {
  return {{"owner", "xenon-launcher-next-module-install"}, {"jobId", id}};
}
// Recover ownership metadata for packages installed before explicit installation roots were saved.
void locate_owned_root(Json &module) {
  if (module.contains("_installRoot") && module.contains("_installOwnerId"))
    return;
  if (!module.value("managed", false))
    return;
  auto path = fs::u8path(module.at("path").get<std::string>());
  for (unsigned depth = 0; depth < 64 && path.has_relative_path();
       ++depth, path = path.parent_path()) {
    if (!fs::is_regular_file(path / "owner.json"))
      continue;
    const auto proof = read_document(path / "owner.json", 4096);
    const auto id = proof.value("jobId", "");
    if (identity(id) && path.filename() == fs::u8path("xenon-" + id) && proof == marker(id)) {
      module["_installRoot"] = path_string(path);
      module["_installOwnerId"] = id;
      return;
    }
  }
  throw std::runtime_error(
      "Managed module ownership could not be established. Unregister it to keep its files.");
}
void inspect_root(const Json &root, bool inspect_tree) {
  const auto path = fs::u8path(root.at("path").get<std::string>());
  const auto id = root.at("ownerId").get<std::string>();
  if (!identity(id) || !path.is_absolute() || path != path.lexically_normal() ||
      path.filename() != fs::u8path("xenon-" + id) || path == path.root_path() ||
      fs::weakly_canonical(path.parent_path()) != path.parent_path())
    throw std::runtime_error("Module installation was redirected; files were left untouched");
  if (!fs::exists(path))
    return; // A prior interrupted deletion may already have removed this recorded, owned root.
  if (redirected_path(path) || fs::weakly_canonical(path) != path || !fs::is_directory(path) ||
      read_document(path / "owner.json", 4096) != marker(id))
    throw std::runtime_error("Module installation ownership changed; files were left untouched");
  if (inspect_tree)
    check_transfer_tree(path);
}
Json locations(const Json &module) {
  Json result = Json::array({module.at("path")});
  for (const auto &previous : module.value("previousVersions", Json::array()))
    result.push_back(previous.at("path"));
  return result;
}
void protect_references(const Json &state, const std::string &id, const Json &roots) {
  const auto protect = [&](const std::string &source) {
    if (source.empty())
      return;
    const auto path = fs::weakly_canonical(fs::absolute(fs::u8path(source)));
    for (const auto &root : roots)
      if (within(path, fs::u8path(root.at("path").get<std::string>())))
        throw std::runtime_error("An installation contains registered game content or another "
                                 "module. Its files were kept.");
  };
  for (const auto &game : state.at("games")) {
    protect(game.value("contentPath", ""));
    for (const auto &medium : game.value("media", Json::array()))
      protect(medium.value("sourcePath", ""));
    for (const auto &dlc : game.value("dlc", Json::array())) {
      protect(dlc.value("path", ""));
      protect(dlc.value("sourcePath", ""));
    }
    for (const auto &copy : game.value("managedCopies", Json::array())) {
      protect(copy.value("root", ""));
      protect(copy.value("originalPath", ""));
    }
  }
  for (const auto &profile : state.at("profiles"))
    for (const auto *key : {"gamePath", "savePath", "screenshotPath"})
      protect(profile.value(key, ""));
  for (auto setting = state.at("settings").begin(); setting != state.at("settings").end();
       ++setting)
    if (setting.key().starts_with("paths/") && setting.value().is_string())
      protect(setting.value().get<std::string>());
  for (const auto &module : state.at("modules"))
    if (module.at("id") != id)
      for (const auto &location : locations(module))
        protect(location.get<std::string>());
}
} // namespace
void LauncherService::require_module_idle(const Json &state, const std::string &id,
                                          const std::string &ignore_job) const {
  if (runtime_) {
    const auto session = runtime_->status();
    const auto status = session.value("state", "idle");
    if (status != "idle" && status != "unavailable" && status != "stopped" && status != "failed" &&
        status != "exited" && status != "crashed")
      throw std::runtime_error("Stop the active session before changing installed modules");
  }
  for (const auto &job : state.at("jobs"))
    if (job.value("id", "") != ignore_job && active(job) &&
        job.value("_command", "") == "modules.uninstall" && job.at("_data").value("id", "") == id)
      throw std::runtime_error("Wait for this module's uninstall to finish");
}
Json LauncherService::prepare_module_uninstall(Json &next, const std::string &id) {
  require_module_idle(next, id);
  for (const auto &job : next.at("jobs"))
    if (active(job) &&
        (job.value("kind", "") == "module.install" || job.value("kind", "") == "module.download" ||
         job.value("_command", "") == "modules.rescan"))
      throw std::runtime_error("Wait for module installation, download and scanning to finish");
  auto found = std::find_if(next["modules"].begin(), next["modules"].end(),
                            [&](const auto &row) { return row.at("id") == id; });
  if (found == next["modules"].end())
    throw std::runtime_error("The module no longer exists");
  Json roots = Json::array();
  std::set<std::string> seen;
  const auto append = [&](Json &version) {
    locate_owned_root(version);
    if (!version.contains("_installRoot"))
      return;
    const auto path = version.at("_installRoot").get<std::string>();
    const auto installed = fs::u8path(version.at("path").get<std::string>());
    if (!within(installed, fs::u8path(path)))
      throw std::runtime_error("Module path no longer belongs to its recorded installation");
    const Json root = {{"path", path}, {"ownerId", version.at("_installOwnerId")}};
    inspect_root(root, false);
    if (seen.insert(path).second)
      roots.push_back(root);
  };
  append(*found);
  if (found->contains("previousVersions"))
    for (auto &version : (*found)["previousVersions"])
      append(version);
  if (roots.empty())
    throw std::runtime_error(
        "This module uses external folders. Unregister it to keep those files.");
  for (const auto &root : roots)
    if (within(storage_, fs::u8path(root.at("path").get<std::string>())))
      throw std::runtime_error(
          "An installation contains the launcher's data folder. Its files were kept.");
  protect_references(next, id, roots);
  return {
      {"id", id}, {"name", found->at("name")}, {"locations", locations(*found)}, {"roots", roots}};
}
void LauncherService::run_module_uninstall(const Json &job,
                                           const std::shared_ptr<std::atomic_bool> &cancel) {
  const auto id = job.at("id").get<std::string>();
  const auto &options = job.at("_data");
  const auto module_id = options.at("id").get<std::string>();
  const auto check = [&] {
    if (cancel->load())
      throw OperationCancelled();
  };
  update_job(id, {{"phase", "Checking owned module files"}, {"progress", 10}});
  for (const auto &root : options.at("roots")) {
    check();
    inspect_root(root, true);
  }
  if (install_observer_)
    install_observer_(id, "Module uninstall ready");
  check();
  {
    std::lock_guard lock(mutex_);
    check();
    require_module_idle(state_, module_id, id);
    const auto found = std::find_if(state_["modules"].begin(), state_["modules"].end(),
                                    [&](const auto &row) { return row.at("id") == module_id; });
    if (found == state_["modules"].end() || locations(*found) != options.at("locations"))
      throw std::runtime_error("Module registration changed before uninstall; files were kept");
    for (const auto &root : options.at("roots"))
      inspect_root(root, false);
    protect_references(state_, module_id, options.at("roots"));
    auto next = state_;
    for (auto &row : next["jobs"])
      if (row.at("id") == id) {
        row["state"] = "committing";
        row["phase"] = "Removing owned module files";
        row["progress"] = 50;
      }
    next["revision"] = state_["revision"].get<std::int64_t>() + 1;
    persist(next);
    state_ = std::move(next);
  }
  // The durable roots remain registered until all deletions succeed. A retry resumes missing roots.
  // Cancellation stops at the commit boundary, before any file is removed.
  for (const auto &root : options.at("roots")) {
    inspect_root(root, true);
    const auto path = fs::u8path(root.at("path").get<std::string>());
    if (!fs::exists(path))
      continue;
    // Keep the marker until last so a partial permission failure is still retryable.
    for (const auto &entry : fs::directory_iterator(path))
      if (entry.path().filename() != "owner.json")
        fs::remove_all(entry.path());
    fs::remove(path / "owner.json");
    try {
      fs::remove(path);
    } catch (...) {
      if (fs::is_directory(path) && !redirected_path(path) && fs::weakly_canonical(path) == path &&
          !fs::exists(path / "owner.json"))
        write_document(path / "owner.json", marker(root.at("ownerId").get<std::string>()));
      throw;
    }
  }
  complete_job(id, [&](Json &next) {
    auto &modules = next["modules"];
    modules.erase(std::remove_if(modules.begin(), modules.end(),
                                 [&](const auto &row) { return row.at("id") == module_id; }),
                  modules.end());
    for (auto &game : next["games"])
      if (game.value("moduleId", "") == module_id) {
        game["moduleId"] = "";
        game["module"] = "No compatible module selected";
        game.erase("tileArt");
        game.erase("heroArt");
      }
    notify(
        next, "Module uninstalled",
        options.at("name").get<std::string>() +
            ": launcher-owned installations removed. External folders and game sources were kept.");
  });
}
} // namespace xenon::launcher_next
