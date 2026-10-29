#include "application/launcher_service.hpp"
#include "core/archive.hpp"
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <thread>
using namespace xenon::launcher_next;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
Json action(LauncherService &service, const std::string &name, Json data = Json::object()) {
  return service.request({{"id", "test"},
                          {"version", 1},
                          {"command", "launcher.action"},
                          {"payload", {{"action", name}, {"data", data}}}});
}
Json require(LauncherService &service, const std::string &name, Json data = Json::object()) {
  auto result = action(service, name, data);
  if (!result.at("ok"))
    throw std::runtime_error(result["error"]["message"].get<std::string>());
  return result.at("data");
}
Json find_job(const Json &snapshot, const std::string &id) {
  for (const auto &job : snapshot.at("jobs"))
    if (job.at("id") == id)
      return job;
  throw std::runtime_error("Job not found");
}
Json finished(LauncherService &service, const std::string &id) {
  for (unsigned i = 0; i < 1000; ++i) {
    const auto job = find_job(service.snapshot(), id);
    const auto state = job.at("state").get<std::string>();
    if (state != "queued" && state != "running" && state != "committing" && state != "cancelling" &&
        !job.value("cleaningUp", false))
      return job;
    std::this_thread::sleep_for(5ms);
  }
  throw std::runtime_error("Installation did not reach a terminal state");
}
void package(const fs::path &path, const std::string &version) {
  write_zip(path, {{"module/module.json", Json({{"id", "jobs.module"},
                                                {"name", "Jobs module"},
                                                {"version", version},
                                                {"settings", Json::array()}})
                                              .dump()},
                   {"module/payload.txt", std::string(256 * 1024, 'x')}});
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool armed = false, entered = false, released = false;
  void arm() {
    std::lock_guard lock(mutex);
    armed = true;
    entered = released = false;
  }
  void observe(const std::string &, const std::string &phase) {
    std::unique_lock lock(mutex);
    if (!armed || phase != "Extracting package")
      return;
    entered = true;
    changed.notify_all();
    if (!changed.wait_for(lock, 5s, [&] { return released; }))
      throw std::runtime_error("Test progress gate timed out");
    armed = false;
  }
  void wait() {
    std::unique_lock lock(mutex);
    check(changed.wait_for(lock, 5s, [&] { return entered; }), "Worker did not reach extraction");
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }
};
int main(int argc, char **argv) {
  const auto root = fs::temp_directory_path() / ("xenon-install-jobs-" + make_id());
  try {
    fs::create_directories(root);
    const auto storage = root / "state";
    const auto installed = root / "installed modules";
    const auto zip = root / "module.zip";
    check(argc == 2, "Supply the packaged settings schema fixture");
    fs::create_directories(storage);
    fs::copy_file(fs::u8path(argv[1]), storage / "settings-schema.json");
    {
      Gate gate;
      LauncherService service(storage, false, {},
                              [&](const auto &id, const auto &phase) { gate.observe(id, phase); });
      require(service, "settings.set",
              {{"key", "paths/modules"}, {"value", path_string(installed)}});
      package(zip, "1");
      auto queued = require(service, "modules.installPackage", {{"path", path_string(zip)}});
      auto id = queued["jobs"].back()["id"].get<std::string>();
      check(queued["jobs"].back()["state"] == "queued",
            "Install request must acknowledge a queued operation");
      check(!queued["jobs"].back().contains("_source") && !queued["jobs"].back().contains("_root"),
            "Private job paths must not leak into snapshots");
      check(finished(service, id)["state"] == "completed", "First package should install");
      const auto old_path = fs::u8path(service.snapshot()["modules"][0]["path"].get<std::string>());
      package(zip, "2");
      gate.arm();
      queued = require(service, "modules.installPackage", {{"path", path_string(zip)}});
      id = queued["jobs"].back()["id"];
      gate.wait();
      check(service.snapshot()["modules"][0]["version"] == "1",
            "Active module must remain registered while extraction runs");
      check(!action(service, "modules.installPackage", {{"path", path_string(zip)}}).at("ok"),
            "Duplicate active package requests must fail");
      require(service, "profiles.create", {{"name", "Edited while installing"}});
      require(service, "settings.set", {{"key", "developer/modeEnabled"}, {"value", true}});
      const auto other = root / "other.zip";
      package(other, "other");
      auto second = require(service, "modules.installPackage", {{"path", path_string(other)}});
      const auto second_id = second["jobs"].back()["id"].get<std::string>();
      require(service, "jobs.cancel", {{"id", second_id}});
      check(find_job(service.snapshot(), second_id)["state"] == "cancelled",
            "Queued cancellation should be immediate");
      require(service, "jobs.cancel", {{"id", id}});
      check(find_job(service.snapshot(), id)["state"] == "cancelling",
            "Running cancellation must acknowledge before cleanup");
      gate.release();
      check(finished(service, id)["state"] == "cancelled", "Running package must cancel");
      check(fs::is_regular_file(old_path / "module.json") &&
                service.snapshot()["modules"][0]["version"] == "1",
            "Cancellation must preserve the old module and its files");
      check(fs::is_regular_file(zip), "Cancellation must preserve the user's package");
      auto retry = require(service, "jobs.retry", {{"id", id}});
      const auto retry_id = retry["jobs"].back()["id"].get<std::string>();
      check(retry_id != id && retry["jobs"].back()["retryOf"] == id,
            "Retry must retain history and use a fresh operation identity");
      check(finished(service, retry_id)["state"] == "completed", "Retried package should complete");
      const auto snapshot = service.snapshot();
      check(snapshot["modules"][0]["version"] == "2" && snapshot["profiles"].size() == 2 &&
                snapshot["settings"]["developer/modeEnabled"] == true,
            "Install commit must preserve concurrent profile/settings edits");
      const auto invalid = root / "invalid.zip";
      std::ofstream(invalid) << "not a zip";
      auto bad = require(service, "modules.installPackage", {{"path", path_string(invalid)}});
      const auto bad_id = bad["jobs"].back()["id"].get<std::string>();
      check(finished(service, bad_id)["state"] == "failed" &&
                service.snapshot()["modules"][0]["version"] == "2",
            "Invalid package must fail without replacing a working module");
      package(invalid, "3");
      auto fixed = require(service, "jobs.retry", {{"id", bad_id}});
      check(finished(service, fixed["jobs"].back()["id"])["state"] == "completed",
            "Retry must use the repaired source's current fingerprint");
      require(service, "jobs.clearFinished");
      check(service.snapshot()["jobs"].empty() &&
                service.snapshot()["modules"][0]["version"] == "3",
            "Clearing operation history must not uninstall modules");
    }
    auto saved = read_document(storage / "state.json");
    const auto current_module = saved["modules"][0]["path"];
    std::vector<std::string> recovery_ids;
    for (int i = 0; i < 3; ++i) {
      const auto id = make_id();
      recovery_ids.push_back(id);
      saved["jobs"].push_back(
          {{"id", id},
           {"kind", "module.install"},
           {"title", "Interrupted test install"},
           {"state", i == 0 ? "committing" : "running"},
           {"progress", 50},
           {"_root", path_string(fs::weakly_canonical(installed))},
           {"_source", path_string(zip)},
           {"_size", fs::file_size(zip)},
           {"_modified", std::to_string(fs::last_write_time(zip).time_since_epoch().count())}});
      const auto directory =
          i == 0 ? installed / ("xenon-" + id) : installed / ".xenon-staging" / id;
      write_document(directory / "owner.json",
                     {{"owner", i == 2 ? "someone-else" : "xenon-launcher-next-module-install"},
                      {"jobId", id}});
      std::ofstream(directory / "preserve-or-clean.txt") << "partial";
    }
    write_document(storage / "state.json", saved);
    write_document(storage / "running.json", {{"startedAt", "crash fixture"}});
    {
      LauncherService recovered(storage);
      check(recovered.snapshot()["recovery"]["previousUncleanExit"],
            "Crash marker must be reported");
      for (const auto &id : recovery_ids)
        check(finished(recovered, id)["state"] == "interrupted",
              "Interrupted work must require an explicit retry");
      check(!fs::exists(installed / ("xenon-" + recovery_ids[0])) &&
                !fs::exists(installed / ".xenon-staging" / recovery_ids[1]),
            "Owned unregistered output and staging must be cleaned after recovery");
      check(fs::is_regular_file(installed / ".xenon-staging" / recovery_ids[2] /
                                "preserve-or-clean.txt") &&
                !finished(recovered, recovery_ids[2]).value("cleanupError", "").empty(),
            "Recovery must leave foreign directories untouched and explain cleanup failure");
      check(recovered.snapshot()["modules"][0]["path"] == current_module &&
                fs::is_regular_file(fs::u8path(current_module.get<std::string>()) / "module.json"),
            "Recovery must preserve the last committed module");
    }
    fs::remove_all(root);
    std::cout << "Async install, cancellation, retry, concurrent edits and interrupted commit "
                 "recovery passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
