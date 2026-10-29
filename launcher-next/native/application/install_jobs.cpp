#include "launcher_service.hpp"
#include "core/archive.hpp"
#include "core/module_settings.hpp"
#include "core/releases.hpp"
#include "picosha2/picosha2.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>

namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
using Clock = std::chrono::steady_clock;
std::string time_now() {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}
bool active(const Json &job) {
  const auto state = job.value("state", "");
  return state == "queued" || state == "running" || state == "cancelling" || state == "committing";
}
bool queued(const Json &job) {
  const auto kind = job.value("kind", "");
  return supported_job_kind(kind) && job.value("state", "") == "queued";
}
Json &job_row(Json &state, const std::string &id) {
  for (auto &job : state["jobs"])
    if (job.value("id", "") == id)
      return job;
  throw std::runtime_error("Operation no longer exists");
}
std::string modified(const fs::path &path) {
  return std::to_string(fs::last_write_time(path).time_since_epoch().count());
}
struct JobPaths {
  fs::path root, stage, installed;
  explicit JobPaths(const Json &job) {
    const auto id = job.at("id").get<std::string>();
    if (id.empty() || id.size() > 100 ||
        id.find_first_not_of("0123456789abcdef-") != std::string::npos)
      throw std::runtime_error("Invalid installation identity");
    root = fs::u8path(job.at("_root").get<std::string>());
    if (!root.is_absolute() || root.lexically_normal() != root ||
        fs::weakly_canonical(root) != root)
      throw std::runtime_error("Installation location changed; retry the package");
    stage = root / ".xenon-staging" / id;
    installed = root / ("xenon-" + id);
  }
};
Json owner(const Json &job) {
  return {{"owner", "xenon-launcher-next-module-install"}, {"jobId", job.at("id")}};
}
void remove_owned(const fs::path &directory, const Json &job) {
  if (!fs::exists(directory))
    return;
  if (fs::weakly_canonical(directory) != directory || fs::is_symlink(directory))
    throw std::runtime_error("Staging directory was redirected; it was left untouched");
  const auto marker = directory / "owner.json";
  if (!fs::exists(marker)) {
    if (fs::is_empty(directory)) {
      fs::remove(directory);
      return;
    }
    throw std::runtime_error("Unrecognised installation directory was left untouched");
  }
  if (read_document(marker, 4096) != owner(job))
    throw std::runtime_error("Installation ownership changed; files were left untouched");
  fs::remove_all(directory);
}
bool referenced(const Json &state, const fs::path &directory) {
  const auto contains = [&](const Json &module) {
    const auto path = fs::u8path(module.value("path", ""));
    const auto relative = path.lexically_relative(directory);
    return !relative.empty() && *relative.begin() != "..";
  };
  for (const auto &module : state["modules"]) {
    if (contains(module))
      return true;
    for (const auto &previous : module.value("previousVersions", Json::array()))
      if (contains(previous))
        return true;
  }
  return false;
}
void move_installation(const fs::path &source, const fs::path &destination) {
  for (unsigned attempt = 0;; ++attempt) {
    std::error_code error;
    fs::rename(source, destination, error);
    if (!error)
      return;
#ifdef _WIN32
    const bool transient = error == std::errc::permission_denied ||
                           error == std::errc::device_or_resource_busy || error.value() == 32 ||
                           error.value() == 33;
    if (transient && attempt < 40) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
#endif
    throw std::runtime_error(
        "Could not move the module into place (" + error.message() +
        "). Close apps using the module folder or check its permissions, then retry.");
  }
}
} // namespace
void LauncherService::enqueue_install(Json &next, const fs::path &input) {
  const auto source = fs::weakly_canonical(fs::absolute(input));
  if (!fs::is_regular_file(source) || fs::file_size(source) > 512ull * 1024 * 1024)
    throw std::runtime_error("Choose a module ZIP smaller than 512 MiB");
  unsigned pending = 0;
  for (const auto &job : next["jobs"])
    if (active(job)) {
      ++pending;
      if (job.value("kind", "") == "module.install" &&
          job.value("_source", "") == path_string(source))
        throw std::runtime_error("This package already has an active installation");
    }
  if (pending >= 32)
    throw std::runtime_error("The operation queue is full");
  while (next["jobs"].size() >= 100) {
    auto &rows = next["jobs"];
    const auto old = std::find_if(rows.begin(), rows.end(), [](const auto &row) {
      return !active(row) && !row.value("_cleanup", false);
    });
    if (old == rows.end())
      break;
    rows.erase(old);
  }
  const auto configured = next["settings"].value("paths/modules", "");
  const auto requested =
      fs::absolute(configured.empty() ? storage_ / "modules" : fs::u8path(configured))
          .lexically_normal();
  fs::create_directories(requested);
  const auto root = fs::weakly_canonical(requested);
  next["jobs"].push_back({{"id", make_id()},
                          {"kind", "module.install"},
                          {"title", "Install " + path_string(source.filename())},
                          {"sourceName", path_string(source.filename())},
                          {"state", "queued"},
                          {"phase", "Waiting"},
                          {"progress", 0},
                          {"createdAt", time_now()},
                          {"_source", path_string(source)},
                          {"_root", path_string(root)},
                          {"_size", fs::file_size(source)},
                          {"_modified", modified(source)}});
}
void LauncherService::recover_install_jobs() {
  bool changed = false;
  for (auto &job : state_["jobs"])
    if (supported_job_kind(job.value("kind", "")) && active(job)) {
      job["state"] = "interrupted";
      job["phase"] = "Interrupted";
      job["error"] = "The operation was interrupted. Previously saved launcher data was preserved. "
                     "Retry when ready.";
      const auto kind = job.value("kind", "");
      job["_cleanup"] = kind == "library.transfer" || kind == "module.install" ||
                        kind == "module.download" || kind == "launcher.download";
      apply_online_error(state_, job,
                         "The previous online operation was interrupted. Retry when ready.");
      job["endedAt"] = time_now();
      changed = true;
    }
  if (changed) {
    state_["revision"] = state_["revision"].get<std::int64_t>() + 1;
    persist(state_);
  }
}
void LauncherService::update_job(const std::string &id, const Json &fields) {
  std::lock_guard lock(mutex_);
  auto next = state_;
  auto &job = job_row(next, id);
  for (auto field = fields.begin(); field != fields.end(); ++field)
    job[field.key()] = field.value();
  next["revision"] = state_["revision"].get<std::int64_t>() + 1;
  persist(next);
  state_ = std::move(next);
}
void LauncherService::cleanup_install(const Json &job) {
  const JobPaths paths(job);
  // Both paths are derived below the recorded module root and must carry this job's marker.
  remove_owned(paths.stage, job);
  bool in_use = false;
  {
    std::lock_guard lock(mutex_);
    in_use = referenced(state_, paths.installed);
  }
  if (!in_use)
    remove_owned(paths.installed, job);
}
void LauncherService::jobs_worker() noexcept {
  for (;;) {
    Json job;
    std::shared_ptr<std::atomic_bool> cancel;
    try {
      bool cleanup = false;
      {
        std::unique_lock lock(mutex_);
        jobs_changed_.wait(lock, [&] {
          return jobs_stopping_ ||
                 std::any_of(state_["jobs"].begin(), state_["jobs"].end(), [](const auto &row) {
                   return row.value("_cleanup", false) || queued(row);
                 });
        });
        if (jobs_stopping_)
          return;
        for (const auto &row : state_["jobs"])
          if (row.value("_cleanup", false)) {
            job = row;
            cleanup = true;
            break;
          }
        if (!cleanup) {
          for (const auto &row : state_["jobs"])
            if (queued(row)) {
              job = row;
              break;
            }
          auto next = state_;
          auto &row = job_row(next, job.at("id"));
          row["state"] = "running";
          row["phase"] = "Starting";
          row["startedAt"] = time_now();
          next["revision"] = state_["revision"].get<std::int64_t>() + 1;
          persist(next);
          state_ = std::move(next);
          running_job_ = job.at("id");
          cancel_job_ = std::make_shared<std::atomic_bool>(false);
          cancel = cancel_job_;
        }
      }
      if (cleanup) {
        std::string error;
        try {
          if (job.value("kind", "") == "module.install")
            cleanup_install(job);
          else if (library_job_kind(job.value("kind", "")))
            cleanup_library(job);
          else
            cleanup_download(job);
        } catch (const std::exception &failure) {
          error = failure.what();
        }
        update_job(job.at("id"), {{"_cleanup", false}, {"cleanupError", error}});
        continue;
      }
      try {
        if (job.value("kind", "") == "module.install")
          run_install(job, cancel);
        else if (library_job_kind(job.value("kind", "")))
          run_library(job, cancel);
        else if (migration_job_kind(job.value("kind", "")))
          run_migration(job, cancel);
        else if (job.value("kind", "") == "service.operation")
          run_service(job, cancel);
        else
          run_network(job, cancel);
      } catch (const std::exception &error) {
        bool stopping = false;
        {
          std::lock_guard lock(mutex_);
          stopping = jobs_stopping_;
        }
        const bool cancelled = dynamic_cast<const OperationCancelled *>(&error) != nullptr;
        const auto kind = job.value("kind", "");
        if (network_job_kind(kind))
          fail_network(job, cancelled ? "Online operation cancelled" : error.what());
        const auto state = stopping ? "interrupted" : cancelled ? "cancelled" : "failed";
        update_job(
            job.at("id"),
            {{"state", state},
             {"phase", stopping    ? "Interrupted"
                       : cancelled ? "Cancelled"
                                   : "Failed"},
             {"error", stopping    ? "Operation paused when the launcher closed. Retry when ready."
                       : cancelled ? "Operation cancelled. Previously saved launcher data was kept."
                                   : error.what()},
             {"endedAt", time_now()},
             {"_cleanup", kind == "library.transfer" || kind == "module.install" ||
                              kind == "module.download" || kind == "launcher.download"}});
      }
      {
        std::lock_guard lock(mutex_);
        running_job_.clear();
        cancel_job_.reset();
      }
    } catch (const std::exception &error) {
      // Preserve the previous registered module even if durable storage itself fails.
      std::lock_guard lock(mutex_);
      if (job.contains("id"))
        try {
          auto &row = job_row(state_, job.at("id"));
          row["state"] = "failed";
          row["phase"] = "Failed";
          row["error"] = std::string("Could not save operation state: ") + error.what();
          row["_cleanup"] = false;
        } catch (...) {
        }
      running_job_.clear();
      cancel_job_.reset();
      std::ofstream(storage_ / "renderer.log", std::ios::app)
          << "Installation worker: " << error.what() << '\n';
    }
  }
}
void LauncherService::run_install(const Json &job,
                                  const std::shared_ptr<std::atomic_bool> &cancel) {
  const JobPaths paths(job);
  const auto id = job.at("id").get<std::string>();
  const auto source = fs::u8path(job.at("_source").get<std::string>());
  const auto check = [&] {
    if (cancel->load())
      throw OperationCancelled();
  };
  check();
  if (!fs::is_regular_file(source) ||
      fs::file_size(source) != job.at("_size").get<std::uintmax_t>() ||
      modified(source) != job.at("_modified").get<std::string>())
    throw std::runtime_error(
        "The source package changed or moved. Retry to use its current contents.");
  fs::create_directories(paths.root);
  fs::create_directories(paths.stage.parent_path());
  if (fs::weakly_canonical(paths.stage.parent_path()) != paths.stage.parent_path() ||
      !fs::create_directory(paths.stage))
    throw std::runtime_error("Cannot create a fresh package staging directory");
  write_document(paths.stage / "owner.json", owner(job));
  auto last = Clock::time_point{};
  std::string last_phase;
  const auto progress = [&](const std::string &phase, int percentage, std::uint64_t done = 0,
                            std::uint64_t total = 0) {
    check();
    const auto now = Clock::now();
    if (phase == last_phase && now - last < std::chrono::milliseconds(200))
      return;
    update_job(
        id,
        {{"phase", phase}, {"progress", percentage}, {"bytesDone", done}, {"bytesTotal", total}});
    last = now;
    last_phase = phase;
    if (install_observer_)
      install_observer_(id, phase);
    check();
  };
  const auto archive = paths.stage / "package.zip";
  picosha2::hash256_one_by_one package_hash;
  {
    progress("Copying package", 1);
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(archive, std::ios::binary);
    if (!input || !output)
      throw std::runtime_error("Cannot copy the module package into staging");
    std::array<char, 256 * 1024> buffer{};
    std::uint64_t copied = 0;
    const auto size = job.at("_size").get<std::uint64_t>();
    while (input) {
      check();
      input.read(buffer.data(), buffer.size());
      const auto count = input.gcount();
      if (count > 0) {
        output.write(buffer.data(), count);
        if (!output)
          throw std::runtime_error("Could not write the staged package");
        copied += count;
        if (job.contains("_sha256"))
          package_hash.process(buffer.data(), buffer.data() + count);
        progress(
            "Copying package",
            1 + static_cast<int>(14. * std::min(copied, size) / std::max<std::uint64_t>(1, size)),
            copied, size);
      }
    }
    output.flush();
    if (!input.eof() || !output || copied != size ||
        modified(source) != job.at("_modified").get<std::string>())
      throw std::runtime_error("The source package changed or could not be copied");
  }
  if (job.contains("_sha256")) {
    package_hash.finish();
    if (picosha2::get_hash_hex_string(package_hash) != job.at("_sha256").get<std::string>())
      throw std::runtime_error("The staged package no longer matches its verified SHA-256 digest");
  }
  const auto payload = paths.stage / "payload";
  extract_package(archive, payload,
                  {[&] { return cancel->load(); },
                   [&](std::uint64_t done, std::uint64_t total) {
                     progress("Extracting package",
                              15 + static_cast<int>(75. * done / std::max<std::uint64_t>(1, total)),
                              done, total);
                   }});
  progress("Checking manifest", 92);
  std::vector<fs::path> candidates;
  for (const auto &entry : fs::recursive_directory_iterator(payload)) {
    check();
    const auto name = path_string(entry.path().filename());
    if (entry.is_regular_file() &&
        (name == "xenon-module.json" || name == "module.json" || name == "manifest.json"))
      if (std::find(candidates.begin(), candidates.end(), entry.path().parent_path()) ==
          candidates.end())
        candidates.push_back(entry.path().parent_path());
  }
  if (candidates.size() != 1)
    throw std::runtime_error("Package must contain exactly one module root");
  Json definition;
  for (const auto *name : {"xenon-module.json", "module.json", "manifest.json"})
    if (fs::is_regular_file(candidates[0] / name)) {
      definition = read_document(candidates[0] / name);
      break;
    }
  (void)module_settings_schema(definition);
  if (job.contains("_expectedModuleId") &&
      definition.value("id", definition.value("moduleId", "")) !=
          job.at("_expectedModuleId").get<std::string>())
    throw std::runtime_error("The downloaded package declares a different module identity");
  if (job.contains("_expectedVersion")) {
    const auto version = Version::parse(definition.value("version", ""));
    if (!version || version->normalized != job.at("_expectedVersion").get<std::string>())
      throw std::runtime_error("The module version does not match its release metadata");
  }
  const auto relative = candidates[0].lexically_relative(paths.stage);
  fs::remove(archive);
  check();
  {
    std::lock_guard lock(mutex_);
    check();
    auto committing = state_;
    auto &row = job_row(committing, id);
    row["state"] = "committing";
    row["phase"] = "Installing";
    row["progress"] = 99;
    committing["revision"] = state_["revision"].get<std::int64_t>() + 1;
    persist(committing);
    state_ = std::move(committing);
  }
  if (fs::exists(paths.installed))
    throw std::runtime_error("The generated installation location already exists");
  // The durable committing record makes this short, uncancellable phase recoverable.
  // Filesystem retries run without holding the state mutex or the command worker.
  move_installation(paths.stage, paths.installed);
  try {
    std::lock_guard lock(mutex_);
    auto next = state_;
    register_installed_module(next, definition, paths.installed / relative);
    auto &completed = job_row(next, id);
    completed["state"] = "completed";
    completed["phase"] = "Completed";
    completed["progress"] = 100;
    completed["endedAt"] = time_now();
    completed["moduleId"] = definition.value("id", definition.value("moduleId", ""));
    next["revision"] = state_["revision"].get<std::int64_t>() + 1;
    persist(next);
    state_ = std::move(next);
  } catch (...) {
    std::error_code ignored;
    fs::rename(paths.installed, paths.stage, ignored);
    throw;
  }
}
} // namespace xenon::launcher_next
