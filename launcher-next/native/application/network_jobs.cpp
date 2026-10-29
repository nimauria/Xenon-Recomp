#include "launcher_service.hpp"
#include "core/archive.hpp"
#include "core/releases.hpp"
#include "core/launcher_update.hpp"
#include "picosha2/picosha2.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
const std::string registry = "https://raw.githubusercontent.com/nimauria/Xenon-Modules/main/";
std::string now() {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}
bool active(const Json &job) {
  const auto state = job.value("state", "");
  return state == "queued" || state == "running" || state == "cancelling" || state == "committing";
}
Json &row(Json &rows, const std::string &id) {
  for (auto &entry : rows)
    if (entry.value("id", "") == id)
      return entry;
  throw std::runtime_error("Catalogue entry or operation no longer exists");
}
Json &update(Json &state, const std::string &id) {
  auto &value = id == "launcher" ? state["launcherUpdate"] : state["moduleUpdates"][id];
  if (!value.is_object())
    value = Json::object();
  return value;
}
std::string installed_version(const Json &state, const std::string &id) {
  if (id == "launcher")
    return launcher_version;
  for (const auto &module : state.at("modules"))
    if (module.at("id") == id)
      return module.value("version", "");
  return {};
}
void history(Json &next, const std::string &module, const std::string &action,
             const std::string &outcome, const std::string &message) {
  next["updateHistory"].push_back({{"id", make_id()},
                                   {"moduleId", module},
                                   {"action", action},
                                   {"outcome", outcome},
                                   {"message", message},
                                   {"time", now()}});
  while (next["updateHistory"].size() > 200)
    next["updateHistory"].erase(next["updateHistory"].begin());
}
Json download_owner(const std::string &id) {
  return {{"owner", "xenon-launcher-next-download"}, {"jobId", id}};
}
} // namespace
bool network_job_kind(const std::string &kind) {
  return kind == "catalog.refresh" || kind == "module.check" || kind == "module.checkAll" ||
         kind == "module.download" || kind == "launcher.check" || kind == "launcher.download";
}
void LauncherService::load_online_state() {
  if (!state_.contains("catalog")) {
    state_["catalog"] = {
        {"state", "idle"}, {"entries", Json::array()}, {"lastError", ""}, {"lastChecked", ""}};
    if (fs::is_regular_file(storage_ / "module-catalog-fallback.json"))
      try {
        state_["catalog"]["entries"].push_back(
            catalog_entry(read_document(storage_ / "module-catalog-fallback.json"),
                          "modules/org.nimauria.project-gracemeria.json"));
        state_["catalog"]["state"] = "bundled";
      } catch (const std::exception &error) {
        state_["catalog"]["lastError"] = error.what();
      }
  }
  if (!state_.contains("moduleUpdates"))
    state_["moduleUpdates"] = Json::object();
  if (!state_.contains("launcherUpdate"))
    state_["launcherUpdate"] = {{"state", "idle"}, {"currentVersion", launcher_version}};
  if (!state_.contains("updateHistory"))
    state_["updateHistory"] = Json::array();
  auto &launcher = state_["launcherUpdate"];
  launcher["currentVersion"] = launcher_version;
  if (launcher.value("state", "") == "preparing" || launcher.value("state", "") == "restarting") {
    if (launcher.value("downloadVersion", "") == launcher_version) {
      launcher["state"] = "up-to-date";
      launcher["lastError"] = "";
    } else {
      launcher["state"] = "error";
      launcher["lastError"] =
          "The previous launcher update did not finish. The current installation was kept.";
      if (launcher.contains("_installPlan"))
        try {
          const auto status =
              launcher_update_status(fs::u8path(launcher.at("_installPlan").get<std::string>()));
          if (status.contains("error"))
            launcher["lastError"] = status.at("error");
        } catch (...) {
        }
    }
  }
}
void LauncherService::project_online(Json &result) const {
  const auto project_update = [&](Json &value, const std::string &id) {
    const auto installed = installed_version(state_, id);
    value["installedVersion"] = installed;
    const auto release = value.value("release", Json());
    const bool asset =
        release.is_object() && release.contains("asset") && release["asset"].is_object();
    const bool newer =
        release.is_object() && newer_version(release.value("version", ""), installed);
    const bool busy =
        std::any_of(state_["jobs"].begin(), state_["jobs"].end(), [&](const auto &job) {
          return active(job) && ((job.value("moduleId", "") == id) ||
                                 (job.value("kind", "") == "module.checkAll" && id != "launcher"));
        });
    bool known = id == "launcher";
    for (const auto &entry : state_["catalog"]["entries"])
      if (entry.at("id") == id && !entry.value("assetName", "").empty())
        known = true;
    value["canCheck"] = http_ && known && !busy && !update_preparing_;
    value["busy"] = busy || (id == "launcher" && update_preparing_);
    value["canDownload"] = http_ && asset && newer &&
                           !release["asset"].value("sha256", "").empty() && !busy &&
                           !update_preparing_;
    const bool staged = asset && !value.value("_packagePath", "").empty() &&
                        value.value("downloadVersion", "") == release.value("version", "") &&
                        value.value("downloadDigest", "") == release["asset"].value("sha256", "");
    value["canInstall"] = (id != "launcher" || !update_installation_.empty()) &&
                          !update_preparing_ && newer && !busy && staged;
    value["updateAvailable"] = !installed.empty() && newer;
    if (!busy && (value.value("state", "") == "ready-to-install" ||
                  value.value("state", "") == "up-to-date")) {
      if (release.is_object() && !newer)
        value["state"] = "up-to-date";
      else if (newer)
        value["state"] = staged ? "ready-to-install" : "update-available";
    }
    for (auto field = value.begin(); field != value.end();)
      if (!field.key().empty() && field.key()[0] == '_')
        field = value.erase(field);
      else
        ++field;
  };
  for (auto &entry : result["catalog"]["entries"]) {
    const auto id = entry.at("id").get<std::string>();
    if (!result["moduleUpdates"].contains(id))
      result["moduleUpdates"][id] = {{"state", "idle"}};
    for (const auto *kind : {"tile", "hero"}) {
      auto &metadata = entry["launcher"];
      const auto file_key = "_" + std::string(kind) + "File",
                 version_key = "_" + std::string(kind) + "Version";
      if (metadata.contains(file_key))
        metadata[std::string(kind) + "Art"] = "xenon://app/catalog-" + std::string(kind) + "/" +
                                              id + "?v=" + metadata.value(version_key, "");
      else
        metadata.erase(std::string(kind) + "Art");
      metadata.erase(file_key);
      metadata.erase(version_key);
    }
  }
  for (auto value = result["moduleUpdates"].begin(); value != result["moduleUpdates"].end();
       ++value)
    project_update(value.value(), value.key());
  project_update(result["launcherUpdate"], "launcher");
}
void LauncherService::enqueue_network(Json &next, const std::string &kind,
                                      const std::string &module_id) {
  if (!http_)
    throw std::runtime_error("Online services are disabled in this launcher session");
  if (!network_job_kind(kind))
    throw std::runtime_error("Unsupported online operation");
  unsigned pending = 0;
  for (const auto &job : next["jobs"])
    if (active(job)) {
      ++pending;
      if (job.value("kind", "") == kind && job.value("moduleId", "") == module_id)
        throw std::runtime_error("This online operation is already active");
    }
  if (pending >= 32)
    throw std::runtime_error("The operation queue is full");
  while (next["jobs"].size() >= 100) {
    auto &jobs = next["jobs"];
    const auto old = std::find_if(jobs.begin(), jobs.end(), [](const auto &job) {
      return !active(job) && !job.value("_cleanup", false);
    });
    if (old == jobs.end())
      break;
    jobs.erase(old);
  }
  Json job = {{"id", make_id()},    {"kind", kind},  {"moduleId", module_id}, {"state", "queued"},
              {"phase", "Waiting"}, {"progress", 0}, {"createdAt", now()}};
  if (kind == "catalog.refresh") {
    job["title"] = "Refresh module catalogue";
    next["catalog"]["state"] = "loading";
    next["catalog"]["lastError"] = "";
  } else if (kind == "module.checkAll") {
    job["title"] = "Check module updates";
    job["_entries"] = Json::array();
    for (const auto &entry : next["catalog"]["entries"])
      if (!entry.value("assetName", "").empty() && !installed_version(next, entry.at("id")).empty())
        job["_entries"].push_back(entry);
  } else {
    const bool launcher = kind.rfind("launcher.", 0) == 0;
    const auto id = launcher ? "launcher" : module_id;
    Json entry = launcher ? Json{{"id", "launcher"},
                                 {"name", "Xenon Launcher"},
                                 {"repository", "nimauria/Xenon-Recomp"},
                                 {"assetName", "xenon-launcher-next-" + host_key() + ".zip"}}
                          : row(next["catalog"]["entries"], id);
    if (entry.value("assetName", "").empty())
      throw std::runtime_error("This module has no release package for " + host_key());
    job["moduleId"] = id;
    job["_entry"] = entry;
    auto &value = update(next, id);
    value["lastError"] = "";
    if (kind == "module.check" || kind == "launcher.check") {
      job["title"] = "Check " + entry.at("name").get<std::string>();
      value["state"] = "checking";
      job["_prerelease"] = next["settings"].value(
          launcher ? "updates/prerelease" : "updates/modulePrerelease", false);
    } else {
      if (!value.contains("release") || !value["release"].is_object() ||
          value["release"]["asset"].is_null() ||
          value["release"]["asset"].value("sha256", "").empty())
        throw std::runtime_error(
            "Check for a release with a SHA-256 package digest before downloading");
      job["title"] = "Download " + entry.at("name").get<std::string>() + " " +
                     value["release"].at("version").get<std::string>();
      job["_release"] = value.at("release");
      value["state"] = "downloading";
    }
    value["jobId"] = job["id"];
  }
  next["jobs"].push_back(job);
}
Json LauncherService::fetch_json(const std::string &url,
                                 const std::shared_ptr<std::atomic_bool> &cancel) {
  std::string body;
  const auto response = http_->get(url, {8 * 1024 * 1024,
                                         "",
                                         [&] { return cancel->load(); },
                                         [&](std::string_view bytes) { body.append(bytes); },
                                         {}});
  if (response.status == 403 || response.status == 429)
    throw std::runtime_error(
        "GitHub is limiting requests. Cached information is retained; try again later.");
  if (response.status < 200 || response.status >= 300)
    throw std::runtime_error("The update service returned HTTP " + std::to_string(response.status));
  return Json::parse(body);
}
void LauncherService::complete_job(const std::string &id,
                                   const std::function<void(Json &)> &mutation, bool finish) {
  std::lock_guard lock(mutex_);
  auto next = state_;
  if (row(next["jobs"], id).value("state", "") == "cancelling")
    throw OperationCancelled();
  mutation(next);
  auto &job = row(next["jobs"], id);
  if (finish) {
    job["state"] = "completed";
    job["phase"] = "Completed";
    job["progress"] = 100;
    job["endedAt"] = now();
  }
  next["revision"] = state_["revision"].get<std::int64_t>() + 1;
  persist(next);
  state_ = std::move(next);
}
void LauncherService::apply_online_error(Json &next, const Json &job, const std::string &message) {
  if (!network_job_kind(job.value("kind", "")))
    return;
  const auto id = job.value("moduleId", "");
  if (job.at("kind") == "catalog.refresh") {
    next["catalog"]["state"] = "offline";
    next["catalog"]["lastError"] = message;
  } else if (!id.empty()) {
    auto &value = update(next, id);
    value["state"] = "error";
    value["lastError"] = message;
    history(next, id, job.at("kind"), "failure", message);
  }
}
void LauncherService::fail_network(const Json &job, const std::string &message) {
  std::lock_guard lock(mutex_);
  auto next = state_;
  apply_online_error(next, job, message);
  next["revision"] = state_["revision"].get<std::int64_t>() + 1;
  persist(next);
  state_ = std::move(next);
}
void LauncherService::cleanup_download(const Json &job) {
  const auto id = job.at("id").get<std::string>();
  if (id.empty() || id.size() > 100 ||
      id.find_first_not_of("0123456789abcdef-") != std::string::npos)
    throw std::runtime_error("Invalid download identity");
  const auto directory = fs::absolute(storage_ / "downloads" / id).lexically_normal();
  if (!fs::exists(directory))
    return;
  if (fs::weakly_canonical(directory) != directory ||
      read_document(directory / "owner.json", 4096) != download_owner(id))
    throw std::runtime_error("Download ownership changed; temporary files were retained");
  {
    std::lock_guard lock(mutex_);
    for (const auto &value : state_["moduleUpdates"])
      if (value.value("_downloadId", "") == id)
        return;
    if (state_["launcherUpdate"].value("_downloadId", "") == id)
      return;
  }
  fs::remove_all(directory);
}
void LauncherService::run_network(const Json &job,
                                  const std::shared_ptr<std::atomic_bool> &cancel) {
  const auto kind = job.at("kind").get<std::string>(), id = job.at("id").get<std::string>();
  const auto cancelled = [&] {
    if (cancel->load())
      throw OperationCancelled();
  };
  cancelled();
  if (kind == "catalog.refresh") {
    update_job(id, {{"phase", "Loading module registry"}, {"progress", 5}});
    const auto paths = catalog_paths(fetch_json(registry + "catalog.json", cancel));
    Json entries = Json::array(), errors = Json::array();
    std::set<std::string> seen;
    std::size_t size = 0;
    for (std::size_t i = 0; i < paths.size(); ++i) {
      cancelled();
      const auto path = paths[i].get<std::string>();
      try {
        auto entry = catalog_entry(fetch_json(registry + path, cancel), path);
        cache_catalog_art(entry, cancel);
        if (!seen.insert(entry.at("id").get<std::string>()).second)
          throw std::runtime_error("Duplicate module identity");
        size += entry.dump().size();
        if (size > 8 * 1024 * 1024)
          throw std::runtime_error("Module catalogue metadata exceeds its size limit");
        entries.push_back(std::move(entry));
      } catch (const OperationCancelled &) {
        throw;
      } catch (const std::exception &error) {
        errors.push_back(path + ": " + error.what());
      }
      update_job(id, {{"phase", "Loading module entries"},
                      {"progress", 5 + static_cast<int>(90 * (i + 1) /
                                                        std::max<std::size_t>(1, paths.size()))}});
    }
    if (!paths.empty() && entries.empty())
      throw std::runtime_error(
          "No module registry entries could be loaded. The cached catalogue was preserved.");
    complete_job(id, [&](Json &next) {
      next["catalog"] = {
          {"state", errors.empty() ? "ready" : "partial"},
          {"entries", entries},
          {"errors", errors},
          {"lastChecked", now()},
          {"lastError", errors.empty() ? "" : "Some module entries could not be loaded"}};
      if (job.value("_checkInstalled", false))
        enqueue_network(next, "module.checkAll");
    });
    return;
  }
  const auto check_release = [&](const Json &entry, bool prerelease) {
    const auto module_id = entry.at("id").get<std::string>();
    const auto release = best_release(fetch_json("https://api.github.com/repos/" +
                                                     entry.at("repository").get<std::string>() +
                                                     "/releases?per_page=30",
                                                 cancel),
                                      entry.at("repository"), entry.at("assetName"), prerelease);
    cancelled();
    std::lock_guard lock(mutex_);
    auto next = state_;
    auto &value = update(next, module_id);
    const auto current = installed_version(next, module_id);
    value["release"] = release;
    value["installedVersion"] = current;
    value["lastChecked"] = now();
    value["lastError"] = "";
    if (release.is_null())
      value["state"] = "no-release";
    else if (release["asset"].is_null())
      value["state"] = "package-unavailable";
    else if (release["asset"].value("sha256", "").empty())
      value["state"] = "digest-unavailable";
    else if (!newer_version(release.at("version"), current))
      value["state"] = "up-to-date";
    else
      value["state"] = current.empty() ? "available" : "update-available";
    history(next, module_id, "check", "success", value.at("state"));
    next["revision"] = state_["revision"].get<std::int64_t>() + 1;
    persist(next);
    state_ = std::move(next);
  };
  if (kind == "module.check" || kind == "launcher.check") {
    update_job(id, {{"phase", "Checking GitHub releases"}, {"progress", 10}});
    check_release(job.at("_entry"), job.value("_prerelease", false));
    complete_job(id, [](Json &) {});
    return;
  }
  if (kind == "module.checkAll") {
    const auto &entries = job.at("_entries");
    for (std::size_t i = 0; i < entries.size(); ++i) {
      cancelled();
      update_job(id, {{"phase", "Checking " + entries[i].at("name").get<std::string>()},
                      {"progress", static_cast<int>(100 * i / entries.size())}});
      bool prerelease = false;
      {
        std::lock_guard lock(mutex_);
        prerelease = state_["settings"].value("updates/modulePrerelease", false);
      }
      try {
        check_release(entries[i], prerelease);
      } catch (const OperationCancelled &) {
        throw;
      } catch (const std::exception &error) {
        auto failed = job;
        failed["moduleId"] = entries[i].at("id");
        fail_network(failed, error.what());
      }
    }
    complete_job(id, [](Json &) {});
    return;
  }
  if (kind == "module.download" || kind == "launcher.download") {
    const auto &release = job.at("_release");
    const auto &asset = release.at("asset");
    const auto expected = sha256_digest(asset.at("sha256"));
    if (expected.empty())
      throw std::runtime_error("Missing SHA-256 package digest");
    const auto directory = fs::absolute(storage_ / "downloads" / id).lexically_normal();
    fs::create_directories(directory.parent_path());
    if (!fs::create_directory(directory) || fs::weakly_canonical(directory) != directory)
      throw std::runtime_error("Could not create a fresh download directory");
    write_document(directory / "owner.json", download_owner(id));
    const auto partial = directory / "package.part", complete = directory / "package.zip";
    picosha2::hash256_one_by_one hash;
    auto last = std::chrono::steady_clock::time_point{};
    update_job(id, {{"phase", "Downloading package"}, {"progress", 1}});
    HttpResponse response;
    {
      std::ofstream output(partial, std::ios::binary);
      if (!output)
        throw std::runtime_error("Could not create a download file");
      response = http_->get(
          asset.at("url"),
          {512ull * 1024 * 1024, "", [&] { return cancel->load(); },
           [&](std::string_view bytes) {
             output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
             if (!output)
               throw std::runtime_error("Could not write the downloaded package");
             hash.process(bytes.begin(), bytes.end());
           },
           [&](std::uint64_t done, std::uint64_t total) {
             const auto current = std::chrono::steady_clock::now();
             if (current - last < std::chrono::milliseconds(200))
               return;
             const auto size = total ? total : asset.at("size").get<std::uint64_t>();
             update_job(id, {{"phase", "Downloading package"},
                             {"progress", static_cast<int>(std::min<std::uint64_t>(
                                              95, 95 * done / std::max<std::uint64_t>(1, size)))},
                             {"bytesDone", done},
                             {"bytesTotal", size}});
             last = current;
           }});
      output.flush();
      if (!output)
        throw std::runtime_error("Could not flush the downloaded package");
    }
    cancelled();
    if (response.status < 200 || response.status >= 300)
      throw std::runtime_error("Package download returned HTTP " + std::to_string(response.status));
    if (response.bytes != asset.at("size").get<std::uint64_t>())
      throw std::runtime_error("The package size does not match its release metadata");
    hash.finish();
    if (picosha2::get_hash_hex_string(hash) != expected)
      throw std::runtime_error("The downloaded package failed SHA-256 verification");
    fs::rename(partial, complete);
    const auto module_id = job.at("moduleId").get<std::string>();
    complete_job(id, [&](Json &next) {
      auto &value = update(next, module_id);
      value["state"] = "ready-to-install";
      value["_packagePath"] = path_string(complete);
      value["_downloadId"] = id;
      value["downloadVersion"] = release.at("version");
      value["downloadDigest"] = expected;
      value["lastError"] = "";
      history(next, module_id, "download", "success", "Package downloaded and SHA-256 verified");
      notify(next, "Package ready",
             job.at("_entry").at("name").get<std::string>() + " " +
                 release.at("version").get<std::string>() + " is ready to install.");
    });
    return;
  }
  throw std::runtime_error("Unsupported network job");
}
} // namespace xenon::launcher_next
