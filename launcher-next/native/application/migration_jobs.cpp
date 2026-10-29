#include "launcher_service.hpp"
#include "core/archive.hpp"
#include "core/legacy_migration.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
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
void check_cancel(const std::shared_ptr<std::atomic_bool> &cancel) {
  if (cancel->load())
    throw OperationCancelled();
}
bool identity(const std::string &id) {
  return !id.empty() && id.size() <= 100 &&
         id.find_first_not_of("0123456789abcdef-") == std::string::npos;
}
void copy_avatar(const fs::path &source, const fs::path &target,
                 const std::shared_ptr<std::atomic_bool> &cancel) {
  if (fs::exists(target))
    throw std::runtime_error(
        "Migration image destination already exists; inspect the legacy data again");
  std::ifstream input(source, std::ios::binary);
  std::ofstream output(target, std::ios::binary);
  input.exceptions(std::ios::badbit);
  output.exceptions(std::ios::badbit | std::ios::failbit);
  if (!input)
    throw std::runtime_error("The previous avatar could not be opened");
  std::array<char, 65536> bytes;
  std::uintmax_t total = 0;
  while (input) {
    check_cancel(cancel);
    input.read(bytes.data(), bytes.size());
    total += input.gcount();
    if (total > 20 * 1024 * 1024)
      throw std::runtime_error("The previous avatar changed during import");
    output.write(bytes.data(), input.gcount());
  }
}
} // namespace
bool migration_job_kind(const std::string &kind) {
  return kind == "migration.inspect" || kind == "migration.apply";
}
void LauncherService::enqueue_migration(Json &next, const std::string &kind, const Json &data) {
  unsigned pending = 0;
  for (const auto &job : next["jobs"])
    if (active(job)) {
      ++pending;
      if (migration_job_kind(job.value("kind", "")))
        throw std::runtime_error("A legacy-data operation is already running");
    }
  if (pending >= 32)
    throw std::runtime_error("The operation queue is full");
  while (next["jobs"].size() >= 100) {
    auto &rows = next["jobs"];
    const auto old = std::find_if(rows.begin(), rows.end(), [](const auto &job) {
      return !active(job) && !job.value("_cleanup", false);
    });
    if (old == rows.end())
      break;
    rows.erase(old);
  }
  Json options = Json::object();
  if (kind == "migration.inspect") {
    options = {{"path", data.value("path", "")},
               {"replaceSettings", data.value("replaceSettings", false)},
               {"activateProfile", data.value("activateProfile", true)}};
  } else {
    const auto id = data.at("id").get<std::string>();
    if (!identity(id) || next.value("migration", Json::object()).value("id", "") != id ||
        next["migration"].value("state", "") != "review")
      throw std::runtime_error("Inspect the previous launcher data before importing it");
    options = {{"id", id}};
  }
  next["jobs"].push_back({{"id", make_id()},
                          {"kind", kind},
                          {"title", kind == "migration.inspect" ? "Review previous launcher data"
                                                                : "Import previous launcher data"},
                          {"state", "queued"},
                          {"phase", "Waiting"},
                          {"progress", 0},
                          {"createdAt", now()},
                          {"_options", options}});
}
void LauncherService::recover_migrations() {
  const auto avatar_root = fs::weakly_canonical(storage_) / "avatars";
  if (fs::exists(avatar_root) &&
      (fs::is_symlink(avatar_root) || fs::weakly_canonical(avatar_root) != avatar_root))
    return;
  const auto directory = storage_ / "migration/journals";
  if (!fs::is_directory(directory))
    return;
  unsigned visited = 0;
  for (const auto &entry : fs::directory_iterator(directory)) {
    if (++visited > 1000)
      break;
    if (!entry.is_regular_file() || entry.is_symlink() || entry.path().extension() != ".json")
      continue;
    try {
      auto journal = read_document(entry.path(), 1024 * 1024);
      const auto id = journal.at("jobId").get<std::string>();
      const auto plan_id = journal.at("planId").get<std::string>();
      if (journal.value("owner", "") != "xenon-launcher-next-migration" || !identity(id) ||
          !identity(plan_id) || entry.path().filename() != fs::u8path(id + ".json") ||
          journal.value("state", "") != "preparing")
        continue;
      bool committed = state_.value("migration", Json::object()).value("appliedJobId", "") == id;
      for (const auto &value : journal.at("avatars")) {
        const auto key = value.get<std::string>();
        if (key.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") != std::string::npos ||
            !key.starts_with("legacy-" + plan_id + "-"))
          continue;
        const bool used =
            std::any_of(state_["profiles"].begin(), state_["profiles"].end(),
                        [&](const auto &p) { return p.value("avatarKey", "") == key; });
        if (!used) {
          const auto file = storage_ / "avatars" / fs::u8path(key);
          if (!fs::is_symlink(file))
            fs::remove(file);
        } else
          committed = true;
      }
      journal["state"] = committed ? "completed" : "interrupted";
      write_document(entry.path(), journal);
    } catch (const std::exception &error) {
      std::ofstream(storage_ / "renderer.log", std::ios::app)
          << "Legacy migration recovery: " << error.what() << '\n';
    }
  }
}
void LauncherService::run_migration(const Json &job,
                                    const std::shared_ptr<std::atomic_bool> &cancel) {
  const auto id = job.at("id").get<std::string>();
  const auto options = job.at("_options");
  const auto root = storage_ / "migration";
  check_cancel(cancel);
  if (job.at("kind") == "migration.inspect") {
    update_job(id, {{"phase", "Reading previous launcher metadata"}, {"progress", 10}});
    const auto input = legacy_snapshot(fs::u8path(options.at("path").get<std::string>()), cancel);
    Json base;
    {
      std::lock_guard lock(mutex_);
      base = state_;
    }
    auto plan =
        legacy_plan(input, base, id, options.at("replaceSettings"), options.at("activateProfile"));
    plan["options"] = options;
    check_cancel(cancel);
    write_document(root / "plan.json", plan);
    complete_job(id, [&](Json &next) {
      check_cancel(cancel);
      next["migration"] = plan.at("summary");
    });
    return;
  }
  const auto plan_id = options.at("id").get<std::string>();
  if (!identity(plan_id))
    throw std::runtime_error("Invalid migration review identity");
  const auto plan = read_document(root / "plan.json");
  if (plan.at("schema") != "xenon.legacy-migration-plan" || plan.at("version") != 1 ||
      plan.at("id") != plan_id)
    throw std::runtime_error("Invalid migration review");
  update_job(id, {{"phase", "Rechecking the reviewed source"}, {"progress", 10}});
  const auto input =
      legacy_snapshot(fs::u8path(plan.at("options").at("path").get<std::string>()), cancel);
  if (input.at("fingerprint") != plan.at("sourceHash"))
    throw std::runtime_error(
        "The previous launcher data changed after review. Inspect it again before importing.");
  Json base;
  {
    std::lock_guard lock(mutex_);
    base = state_;
  }
  if (migration_target_hash(base) != plan.at("targetHash").get<std::string>())
    throw std::runtime_error(
        "Your new launcher data changed after review. Inspect it again to retain those changes.");
  const auto backup = root / "backups" / fs::u8path(id + ".json");
  write_document(backup, base);
  Json journal = {{"owner", "xenon-launcher-next-migration"},
                  {"jobId", id},
                  {"planId", plan_id},
                  {"state", "preparing"},
                  {"backup", path_string(backup)},
                  {"avatars", Json::array()}};
  for (const auto &avatar : plan.at("avatars"))
    journal["avatars"].push_back(avatar.at("key"));
  const auto journal_path = root / "journals" / fs::u8path(id + ".json");
  write_document(journal_path, journal);
  try {
    fs::create_directories(storage_ / "avatars");
    const auto avatar_root = fs::weakly_canonical(storage_) / "avatars";
    if (fs::is_symlink(avatar_root) || fs::weakly_canonical(avatar_root) != avatar_root)
      throw std::runtime_error(
          "The launcher avatar folder was redirected; files were left untouched");
    for (const auto &avatar : plan.at("avatars")) {
      check_cancel(cancel);
      const auto key = avatar.at("key").get<std::string>();
      if (!key.starts_with("legacy-" + plan_id + "-") ||
          fs::u8path(key).filename() != fs::u8path(key) || key.find('\\') != std::string::npos)
        throw std::runtime_error("Invalid migration avatar identity");
      const auto destination = storage_ / "avatars" / fs::u8path(key);
      copy_avatar(fs::u8path(avatar.at("path").get<std::string>()), destination, cancel);
      if (migration_file_hash(destination, cancel) != avatar.at("sha256").get<std::string>())
        throw std::runtime_error("The previous avatar changed while being copied");
    }
    if (install_observer_)
      install_observer_(id, "migration.before-commit");
    check_cancel(cancel);
    update_job(
        id,
        {{"state", "committing"}, {"phase", "Saving migrated launcher data"}, {"progress", 95}});
    complete_job(id, [&](Json &next) {
      if (migration_target_hash(next) != plan.at("targetHash").get<std::string>())
        throw std::runtime_error("Your launcher data changed during import. Inspect again; the "
                                 "newer state was retained.");
      for (auto field = plan.at("patch").begin(); field != plan.at("patch").end(); ++field)
        if (field.key() != "lastPage" || !next.contains("lastPage") ||
            plan.at("options").value("replaceSettings", false))
          next[field.key()] = field.value();
      next["migration"] = plan.at("summary");
      next["migration"]["state"] = "completed";
      next["migration"]["appliedJobId"] = id;
      next["migration"]["backupPath"] = path_string(backup);
      notify(next, "Previous launcher data imported",
             "Profiles, settings and library/module references were imported. The previous "
             "launcher files were retained.");
    });
    journal["state"] = "completed";
    try {
      write_document(journal_path, journal);
    } catch (const std::exception &) { /* Recovery can recognize the committed state. */
    }
  } catch (...) {
    std::lock_guard lock(mutex_);
    recover_migrations();
    throw;
  }
}
} // namespace xenon::launcher_next
