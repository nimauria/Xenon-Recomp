#include "launcher_service.hpp"
#include "core/archive.hpp"
#include "core/file_transfer.hpp"
#include "core/dlc_catalog.hpp"
#include "core/game_media.hpp"
#include "core/game_activity.hpp"
#include <algorithm>
#include <chrono>
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
std::string now() {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}
bool active(const Json &job) {
  const auto status = job.value("state", "");
  return status == "queued" || status == "running" || status == "cancelling" ||
         status == "committing";
}
Json &row(Json &rows, const std::string &id) {
  for (auto &value : rows)
    if (value.value("id", "") == id)
      return value;
  throw std::runtime_error("The game or operation no longer exists");
}
bool within(const fs::path &path, const fs::path &root) {
  const auto relative = path.lexically_relative(root);
  return !relative.empty() && *relative.begin() != "..";
}
Json owner(const Json &job) {
  return {{"owner", "xenon-launcher-next-library"},
          {"jobId", job.at("_ownerId")},
          {"gameId", job.at("_gameId")}};
}
struct Paths {
  fs::path root, stage, installed;
  explicit Paths(const Json &job) {
    const auto id = job.at("_ownerId").get<std::string>();
    if (id.empty() || id.size() > 100 ||
        id.find_first_not_of("0123456789abcdef-") != std::string::npos)
      throw std::runtime_error("Invalid managed content identity");
    root = fs::u8path(job.at("_root").get<std::string>());
    if (!root.is_absolute() || root != root.lexically_normal() ||
        fs::weakly_canonical(root) != root || redirected_path(root))
      throw std::runtime_error("Managed library location changed; files were left untouched");
    stage = root / ".xenon-staging" / ("library-" + id);
    installed = root / ("xenon-" + id);
  }
};
void remove_owned(const fs::path &path, const Json &job) {
  if (!fs::exists(path))
    return;
  if (fs::weakly_canonical(path) != path || redirected_path(path) ||
      read_document(path / "owner.json", 4096) != owner(job))
    throw std::runtime_error("Managed content ownership changed; files were left untouched");
  check_transfer_tree(path);
  fs::remove_all(path);
}
} // namespace
bool library_job_kind(const std::string &kind) {
  return kind == "library.transfer" || kind == "library.deleteManaged" ||
         kind == "library.verifyManaged";
}
bool supported_job_kind(const std::string &kind) {
  return kind == "module.install" || network_job_kind(kind) || migration_job_kind(kind) ||
         kind == "service.operation" || library_job_kind(kind);
}
void LauncherService::require_library_idle(const Json &state, const std::string &game_id) const {
  for (const auto &job : state.at("jobs"))
    if (library_job_kind(job.value("kind", "")) && job.value("_gameId", "") == game_id &&
        active(job))
      throw std::runtime_error("Wait for this game's file operation to finish");
  // Runtime owns the live session; inspect the same adapter snapshot used by the launcher.
  const auto current = runtime_ ? runtime_->status() : Json::object();
  if (current.is_object() && current.value("gameId", "") == game_id) {
    const auto status = current.value("state", "");
    if (status != "failed" && status != "stopped" && status != "exited" && status != "crashed" &&
        status != "unavailable" && status != "idle")
      throw std::runtime_error("Stop the game before changing its files");
  }
}
void LauncherService::enqueue_library(Json &next, const std::string &kind, const Json &data) {
  const auto type = data.value("type", "game");
  const auto mode = data.value("mode", "copy");
  if (type != "game" && type != "dlc")
    throw std::runtime_error("Unknown library transfer type");
  if (mode != "copy" && mode != "move")
    throw std::runtime_error("Unknown library transfer mode");
  if (type == "dlc" && mode != "copy")
    throw std::runtime_error("DLC imports preserve their original files");
  const auto game_id = data.value("id", make_id());
  if (data.contains("id")) {
    row(next["games"], game_id);
    auto checking = next;
    if (kind == "library.transfer") {
      auto &jobs = checking["jobs"];
      jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                                [](const auto &value) {
                                  return value.value("kind", "") == "library.transfer";
                                }),
                 jobs.end());
    }
    require_library_idle(checking, game_id);
  }
  unsigned pending = 0;
  for (const auto &job : next["jobs"])
    if (active(job))
      ++pending;
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
  Json job = {{"id", make_id()}, {"kind", kind},       {"state", "queued"}, {"phase", "Waiting"},
              {"progress", 0},   {"createdAt", now()}, {"_options", data},  {"_gameId", game_id},
              {"_type", type},   {"_mode", mode}};
  if (kind == "library.deleteManaged" || kind == "library.verifyManaged") {
    const auto &game = row(next["games"], game_id);
    auto copies = game.value("managedCopies", Json::array());
    const auto dlc_id = data.value("dlcId", "");
    const auto media_id = data.value("mediaId", "");
    if (!media_id.empty())
      copies.erase(std::remove_if(copies.begin(), copies.end(),
                                  [&](const auto &copy) {
                                    return copy.value("_type", "") != "game" ||
                                           copy.value("entryId", "") != media_id;
                                  }),
                   copies.end());
    if (!dlc_id.empty())
      copies.erase(std::remove_if(copies.begin(), copies.end(),
                                  [&](const auto &copy) {
                                    return copy.value("entryId", "") != dlc_id ||
                                           (kind == "library.verifyManaged" &&
                                            copy.value("previous", false));
                                  }),
                   copies.end());
    if (copies.empty())
      throw std::runtime_error("No launcher-owned files are registered for this selection");
    job["_copies"] = copies;
    job["title"] =
        kind == "library.deleteManaged" ? "Delete managed files" : "Check managed import receipt";
  } else {
    const auto requested =
        fs::absolute(fs::u8path(data.at("path").get<std::string>())).lexically_normal();
    if (redirected_path(requested))
      throw std::runtime_error("Choose original content rather than a filesystem link");
    const auto source = fs::canonical(requested);
    if (source == source.root_path())
      throw std::runtime_error("Choose a game folder, not a drive root");
    if (type == "game" && !fs::is_directory(source)) {
      auto extension = path_string(source.extension());
      std::transform(extension.begin(), extension.end(), extension.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      if (extension != ".xex" && extension != ".iso" && extension != ".xgd" && extension != ".dvd")
        throw std::runtime_error("Choose a game folder, XEX or disc image");
    }
    auto configured = next["settings"].value("paths/games", "");
    auto root = fs::absolute(configured.empty() ? storage_ / "games" : fs::u8path(configured))
                    .lexically_normal();
    fs::create_directories(root);
    root = fs::canonical(root);
    if (within(root, source) || within(source, root) || within(storage_, source) ||
        within(source, storage_))
      throw std::runtime_error("Game sources and the managed library must be separate folders");
    for (const auto &other : next["jobs"])
      if (active(other) && other.value("_source", "") == path_string(source))
        throw std::runtime_error("This source already has an active transfer");
    std::vector<fs::path> sources;
    for (const auto &value : data.value("paths", Json::array({path_string(source)}))) {
      const auto requested_source =
          fs::absolute(fs::u8path(value.get<std::string>())).lexically_normal();
      if (redirected_path(requested_source))
        throw std::runtime_error("Add-on source is redirected");
      const auto part = fs::canonical(requested_source);
      if (within(root, part) || within(part, root) || within(storage_, part) ||
          within(part, storage_))
        throw std::runtime_error("Content sources and managed storage must be separate");
      sources.push_back(part);
    }
    if (sources.empty() || sources.size() > 128 || (type != "dlc" && sources.size() != 1) ||
        sources.front() != source)
      throw std::runtime_error("Invalid content source selection");
    job["_sources"] = Json::array();
    for (const auto &part : sources)
      job["_sources"].push_back(path_string(part));
    if (data.contains("catalogId")) {
      if (type != "dlc")
        throw std::runtime_error("Catalogue add-on selection requires a DLC import");
      job["_catalog"] = selected_dlc_definition(next, game_id, data.at("catalogId"));
      const auto &game = row(next["games"], game_id);
      job["_moduleId"] = game.value("moduleId", "");
      for (const auto &other : next["jobs"])
        if (active(other) && other.value("_gameId", "") == game_id && other.contains("_catalog") &&
            other.at("_catalog").at("dlcId") == job.at("_catalog").at("dlcId"))
          throw std::runtime_error("This add-on already has an active import");
      if (game.value("managedCopies", Json::array()).size() >= 128)
        throw std::runtime_error("Remove old managed copies before importing more add-on content");
    }
    job["_source"] = path_string(source);
    job["_root"] = path_string(root);
    job["_ownerId"] = job.at("id");
    job["_entryId"] = make_id();
    if (type == "game" && data.contains("id")) {
      auto &game = row(next["games"], game_id);
      normalize_game_media(game);
      for (const auto &medium : game.at("media"))
        if (same_media_source(medium.at("sourcePath"), path_string(source)))
          job["_entryId"] = medium.at("id");
      if (data.contains("mediaId") && data.at("mediaId") != job.at("_entryId"))
        throw std::runtime_error("The media reference no longer matches this source");
    }
    if (job.contains("_catalog"))
      for (const auto &dlc : row(next["games"], game_id).at("dlc"))
        if (dlc.value("catalogId", "") == data.at("catalogId").get<std::string>())
          job["_entryId"] = dlc.at("id");
    job["title"] = std::string(mode == "move" ? "Move " : "Copy ") + path_string(source.filename());
    if (job.contains("_catalog"))
      job["title"] = "Copy " + job.at("_catalog").at("name").get<std::string>();
  }
  next["jobs"].push_back(job);
}
void LauncherService::cleanup_library(const Json &job) {
  if (job.value("kind", "") != "library.transfer")
    return;
  const Paths paths(job);
  remove_owned(paths.stage, job);
  bool registered = false;
  {
    std::lock_guard lock(mutex_);
    for (const auto &game : state_["games"])
      for (const auto &copy : game.value("managedCopies", Json::array()))
        if (copy.value("root", "") == path_string(paths.installed))
          registered = true;
  }
  if (!registered)
    remove_owned(paths.installed, job);
}
void LauncherService::run_library(const Json &job,
                                  const std::shared_ptr<std::atomic_bool> &cancel) {
  const auto id = job.at("id").get<std::string>(), game_id = job.at("_gameId").get<std::string>();
  const auto check = [&] {
    if (cancel->load())
      throw OperationCancelled();
  };
  if (job.at("kind") == "library.verifyManaged") {
    update_job(id, {{"phase", "Checking imported file hashes"}, {"progress", 10}});
    for (const auto &copy : job.at("_copies")) {
      check();
      const Paths paths(copy);
      if (read_document(paths.installed / "owner.json", 4096) != owner(copy))
        throw std::runtime_error("Managed content ownership changed");
      verify_library_content(
          paths.installed / "content",
          read_document(paths.installed / "copy-manifest.json", 64 * 1024 * 1024), check);
    }
    complete_job(id, [&](Json &next) {
      auto &game = row(next["games"], game_id);
      for (const auto &copy : job.at("_copies"))
        if (copy.at("_type") == "dlc") {
          auto &dlc = row(game["dlc"], copy.at("entryId"));
          dlc["receiptVerified"] = true;
          dlc["lastCheckedAt"] = now();
        }
      notify(next, "Managed copy checked",
             "Files match their local import receipt. Xenon content validity and compatibility "
             "have not been verified.");
    });
    return;
  }
  if (job.at("kind") == "library.deleteManaged") {
    // Validate every selected root before deleting the first one.
    for (const auto &copy : job.at("_copies")) {
      check();
      const Paths paths(copy);
      if (fs::exists(paths.installed)) {
        if (read_document(paths.installed / "owner.json", 4096) != owner(copy))
          throw std::runtime_error("Managed ownership changed");
        check_transfer_tree(paths.installed);
      }
    }
    update_job(id, {{"state", "committing"}, {"phase", "Deleting owned files"}});
    for (const auto &copy : job.at("_copies")) {
      const Paths paths(copy);
      remove_owned(paths.installed, copy);
    }
    complete_job(id, [&](Json &next) {
      auto &game = row(next["games"], game_id);
      for (const auto &copy : job.at("_copies")) {
        auto &copies = game["managedCopies"];
        copies.erase(
            std::remove_if(copies.begin(), copies.end(),
                           [&](const auto &value) { return value.at("root") == copy.at("root"); }),
            copies.end());
        if (copy.at("_type") == "dlc") {
          auto &dlc = game["dlc"];
          dlc.erase(std::remove_if(
                        dlc.begin(), dlc.end(),
                        [&](const auto &value) { return value.at("id") == copy.at("entryId"); }),
                    dlc.end());
        } else {
          const auto original = fs::u8path(copy.at("originalPath").get<std::string>());
          auto &medium = game_media(game, copy.at("entryId"));
          if (fs::exists(original)) {
            medium["sourcePath"] = path_string(original);
            medium["importMode"] = "kept";
            medium["missing"] = false;
          } else
            medium["missing"] = true;
          sync_game_media(game);
          game["verified"] = false;
        }
      }
      notify(next, "Library storage",
             "Managed files deleted. Original sources and the library entry were preserved.");
    });
    return;
  }
  const Paths paths(job);
  const auto source = fs::u8path(job.at("_source").get<std::string>());
  const bool folder = job.value("_sources", Json::array()).size() > 1 ? true
                      : job.value("_committed", false)
                          ? read_document(paths.installed / "copy-manifest.json", 64 * 1024 * 1024)
                                .at("folder")
                                .get<bool>()
                          : fs::is_directory(source);
  const auto content =
      folder ? paths.installed / "content" : paths.installed / "content" / source.filename();
  if (!job.value("_committed", false)) {
    check();
    fs::create_directories(paths.stage.parent_path());
    if (fs::weakly_canonical(paths.stage.parent_path()) != paths.stage.parent_path() ||
        !fs::create_directory(paths.stage))
      throw std::runtime_error("Cannot create a fresh content staging folder");
    write_document(paths.stage / "owner.json", owner(job));
    auto last = std::chrono::steady_clock::time_point{};
    std::vector<fs::path> sources;
    for (const auto &value : job.value("_sources", Json::array({path_string(source)})))
      sources.push_back(fs::u8path(value.get<std::string>()));
    const auto manifest = copy_library_sources(
        sources, paths.stage / "content", check, [&](std::uint64_t done, std::uint64_t total) {
          const auto time = std::chrono::steady_clock::now();
          if (time - last < std::chrono::milliseconds(200))
            return;
          update_job(id, {{"phase", "Copying and verifying content"},
                          {"progress",
                           int(90. * std::min(done, total) / std::max<std::uint64_t>(1, total))},
                          {"bytesDone", done},
                          {"bytesTotal", total}});
          if (install_observer_)
            install_observer_(id, "Copying content");
          last = time;
        });
    if (job.at("_type") == "dlc" && manifest.at("files").empty())
      throw std::runtime_error("The add-on source contains no payload files");
    write_document(paths.stage / "copy-manifest.json", manifest);
    check();
    fs::rename(paths.stage, paths.installed);
    if (install_observer_)
      install_observer_(id, "Content ready");
    check();
    complete_job(
        id,
        [&](Json &next) {
          if (!job.at("_options").contains("id"))
            next["games"].push_back(
                {{"id", game_id},
                 {"title", path_string(source.stem())},
                 {"module", "No compatible module selected"},
                 {"moduleId", ""},
                 {"description",
                  "Managed local content. Compatibility requires the Xenon content adapter."},
                 {"favorite", false},
                 {"lastPlayed", nullptr},
                 {"addedAt", now()},
                 {"dlc", Json::array()},
                 {"verified", false}});
          auto &game = row(next["games"], game_id);
          if (job.contains("_catalog")) {
            if (game.value("moduleId", "") != job.at("_moduleId").get<std::string>() ||
                selected_dlc_definition(next, game_id, job.at("_catalog").at("dlcId")) !=
                    job.at("_catalog"))
              throw std::runtime_error(
                  "The selected module or add-on definition changed during import");
            if (game.contains("managedCopies"))
              for (auto &previous : game["managedCopies"])
                if (previous.at("entryId") == job.at("_entryId"))
                  previous["previous"] = true;
          }
          if (!game.contains("managedCopies"))
            game["managedCopies"] = Json::array();
          Json copy;
          for (const auto *key : {"_ownerId", "_gameId", "_root", "_type"})
            copy[key] = job.at(key);
          copy["root"] = path_string(paths.installed);
          copy["entryId"] = job.at("_entryId");
          copy["originalPath"] = path_string(source);
          copy["originalRetained"] = true;
          game["managedCopies"].push_back(copy);
          if (job.at("_type") == "dlc") {
            Json entry = {{"id", job.at("_entryId")},
                          {"name", path_string(source.filename())},
                          {"path", path_string(content)},
                          {"verified", false},
                          {"managed", true}};
            if (job.contains("_catalog")) {
              entry["catalogId"] = job.at("_catalog").at("dlcId");
              entry["name"] = job.at("_catalog").at("name");
              entry["contentIds"] = job.at("_catalog").at("contentIds");
              entry["version"] = job.at("_catalog").at("version");
              entry["catalogSelectionVerified"] = false;
            }
            const auto previous =
                std::find_if(game["dlc"].begin(), game["dlc"].end(), [&](const auto &value) {
                  return value.at("id") == job.at("_entryId");
                });
            if (previous == game["dlc"].end())
              game["dlc"].push_back(entry);
            else
              *previous = entry;
          } else {
            if (!game.contains("media")) {
              game["media"] = Json::array();
              game["mediaModelVersion"] = 1;
            }
            auto medium =
                std::find_if(game["media"].begin(), game["media"].end(), [&](const auto &value) {
                  return value.at("id") == job.at("_entryId");
                });
            if (medium == game["media"].end()) {
              auto record = make_game_media(path_string(content));
              record["id"] = job.at("_entryId");
              record["importMode"] = "copied";
              record["isPrimary"] = game["media"].empty();
              game["media"].push_back(record);
            } else {
              (*medium)["sourcePath"] = path_string(content);
              (*medium)["importMode"] = "copied";
            }
            sync_game_media(game);
            game_media(game, job.at("_entryId"))["missing"] = false;
            game["verified"] = false;
          }
          normalize_game_activity(game);
          auto &operation = row(next["jobs"], id);
          operation["_committed"] = true;
          operation["phase"] = "Managed copy saved";
        },
        false);
    if (install_observer_)
      install_observer_(id, "Content saved");
  }
  check();
  bool removed = false;
  if (job.at("_mode") == "move") {
    update_job(id, {{"phase", "Removing verified original files"}, {"progress", 95}});
    const auto manifest = read_document(paths.installed / "copy-manifest.json", 64 * 1024 * 1024);
    removed = remove_transferred_source(source, paths.installed / "content", manifest, check);
  }
  complete_job(id, [&](Json &next) {
    auto &game = row(next["games"], game_id);
    for (auto &copy : game["managedCopies"])
      if (copy.at("root") == path_string(paths.installed))
        copy["originalRetained"] = !removed;
    if (job.at("_type") == "game")
      game_media(game, job.at("_entryId"))["importMode"] = removed ? "moved" : "copied";
    if (job.at("_mode") == "move" && !removed)
      row(next["jobs"], id)["warning"] =
          "Managed copy is ready; changed or unavailable original files were retained.";
    notify(next, "Library storage",
           job.at("_type") == "dlc" ? "DLC copied into the library. Original files were kept."
           : removed                ? "Game moved into the managed library."
                                    : "Managed game copy is ready. Original files were kept.");
  });
}
} // namespace xenon::launcher_next
