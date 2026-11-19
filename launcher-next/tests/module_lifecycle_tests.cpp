#include "application/launcher_service.hpp"
#include "core/archive.hpp"
#include "core/module_manifest.hpp"
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <thread>
using namespace xenon::launcher_next;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class Operation> void rejects(Operation operation, const char *message) {
  bool failed = false;
  try {
    operation();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed, message);
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
    throw std::runtime_error(result.at("error").at("message").get<std::string>());
  return result.at("data");
}
Json finished(LauncherService &service, const std::string &id) {
  for (unsigned i = 0; i < 1000; ++i) {
    const auto state = service.snapshot();
    for (const auto &job : state.at("jobs"))
      if (job.at("id") == id && job.at("state") != "queued" && job.at("state") != "running" &&
          job.at("state") != "committing" && job.at("state") != "cancelling")
        return job;
    std::this_thread::sleep_for(5ms);
  }
  throw std::runtime_error("Module lifecycle job timed out");
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
    if (!armed || phase != "Module uninstall ready")
      return;
    entered = true;
    changed.notify_all();
    if (!changed.wait_for(lock, 5s, [&] { return released; }))
      throw std::runtime_error("Uninstall gate timeout");
    armed = false;
  }
  void wait() {
    std::unique_lock lock(mutex);
    check(changed.wait_for(lock, 5s, [&] { return entered; }),
          "Uninstall preflight was not reached");
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }
};
void package(const fs::path &path, const std::string &version) {
  write_zip(path, {{"module/module.json", Json({{"id", "lifecycle.module"},
                                                {"name", "Lifecycle module"},
                                                {"version", version},
                                                {"runtime_apis", {{"input", {{"version", 1}}}}}})
                                              .dump()},
                   {"module/content/Game.iso", "module fixture data"}});
}
int main(int argc, char **argv) {
  const auto root = fs::temp_directory_path() / ("xenon-module-lifecycle-" + make_id());
  try {
    check(argc == 3, "Supply settings schema and fixture host");
    fs::create_directories(root / "state");
    fs::copy_file(fs::u8path(argv[1]), root / "state/settings-schema.json");
    for (const auto &manifest : {Json{{"runtimeApis", {{"input", {{"version", 1}}}}}},
                                 Json{{"runtime_apis", {{"input", {{"version", 1}}}}}},
                                 Json{{"runtime", {{"apis", {{"input", {{"version", 1}}}}}}}}})
      check(module_runtime_apis(manifest).at("input").at("required") == true,
            "All legacy API aliases must normalize required defaults");
    rejects([&] { module_runtime_apis({{"runtimeApis", {{"input", {{"version", 1.5}}}}}}); },
            "Fractional API versions must fail");
    rejects([&] { module_identity({{"id", "unsafe..module"}}); },
            "Unsafe IDs must fail across registration paths");
    const Json base = {{"id", "compat.module"}};
    auto manifest = base;
    manifest["runtimeApis"] = {{"input", {{"version", 2}}}};
    check(module_compatibility(manifest, root).at("status") == "incompatible",
          "Unsupported required input APIs must be identified");
    manifest["runtimeApis"]["input"]["required"] = false;
    check(module_compatibility(manifest, root).at("issues").empty(),
          "Optional newer APIs must remain warnings");
    manifest = base;
    manifest["nativeExtension"] = "missing.dll";
    check(module_compatibility(manifest, root).at("issues").size() == 1,
          "Missing extensions must be reported without loading binaries");
    manifest["nativeExtension"] = "../outside.dll";
    rejects([&] { module_compatibility(manifest, root); },
            "Native extensions must not escape their module root");
    manifest = base;
    manifest["runtime"]["apis"]["future"] = {{"version", 1}};
    check(module_compatibility(manifest, root).at("status") == "unverified",
          "Unknown providers must never be described as verified");
    const auto zip = root / "module.zip", storage = root / "state", installed = root / "installed",
               game = root / "Outside.iso";
    std::ofstream(game) << "original game fixture";
    Json interrupted;
    fs::path current_root, previous_root;
    {
      Gate gate;
      LauncherService service(
          storage, false, fs::u8path(argv[2]),
          [&](const auto &id, const auto &phase) { gate.observe(id, phase); }, nullptr);
      require(service, "settings.set",
              {{"key", "paths/modules"}, {"value", path_string(installed)}});
      for (const auto *version : {"1", "2"}) {
        package(zip, version);
        const auto queued =
            require(service, "modules.installPackage", {{"path", path_string(zip)}});
        check(finished(service, queued.at("jobs").back().at("id")).at("state") == "completed",
              "Fixture packages must install");
      }
      auto state = service.snapshot();
      current_root = fs::u8path(state.at("modules")[0].at("path").get<std::string>())
                         .parent_path()
                         .parent_path();
      previous_root =
          fs::u8path(state.at("modules")[0].at("previousVersions")[0].at("path").get<std::string>())
              .parent_path()
              .parent_path();
      check(state.at("modules")[0].at("canUninstall") == true &&
                !state.at("modules")[0].contains("_installRoot"),
            "Owned uninstall availability must not expose ownership metadata");
      require(service, "modules.rollback", {{"id", "lifecycle.module"}});
      check(service.snapshot().at("modules")[0].at("previousVersions").size() == 1,
            "Rollback must retain the displaced owned version");
      require(service, "modules.rollback", {{"id", "lifecycle.module"}});
      const auto original_marker = read_document(previous_root / "owner.json");
      write_document(previous_root / "owner.json", {{"owner", "foreign"}});
      check(!action(service, "modules.uninstall", {{"id", "lifecycle.module"}}).at("ok"),
            "Foreign markers must reject deletion before queueing");
      check(fs::exists(current_root / "payload/module/module.json"),
            "Foreign ownership must preserve every installation");
      write_document(previous_root / "owner.json", original_marker);
      require(service, "library.import",
              {{"paths",
                Json::array({path_string(current_root / "payload/module/content/Game.iso")})}});
      state = service.snapshot();
      const auto embedded_id = state.at("games")[0].at("id");
      check(!action(service, "modules.uninstall", {{"id", "lifecycle.module"}}).at("ok"),
            "Registered content inside module folders must be protected");
      require(service, "library.remove", {{"id", embedded_id}});
      require(service, "library.import", {{"paths", Json::array({path_string(game)})}});
      state = service.snapshot();
      const auto game_id = state.at("games")[0].at("id");
      require(service, "library.update", {{"id", game_id}, {"moduleId", "lifecycle.module"}});
      const auto definition_path = current_root / "payload/module/module.json";
      const auto original_definition = read_document(definition_path);
      auto incompatible = original_definition;
      incompatible["runtime_apis"]["input"]["version"] = 2;
      write_document(definition_path, incompatible);
      require(service, "modules.verify", {{"id", "lifecycle.module"}});
      check(service.snapshot().at("modules")[0].at("compatibility").at("status") ==
                    "incompatible" &&
                !action(service, "session.launch", {{"gameId", game_id}}).at("ok"),
            "Required unsupported APIs must be visible and block fixture handoff");
      write_document(definition_path, original_definition);
      require(service, "modules.verify", {{"id", "lifecycle.module"}});
      require(service, "session.launch", {{"gameId", game_id}});
      check(!action(service, "modules.uninstall", {{"id", "lifecycle.module"}}).at("ok"),
            "Active runtime sessions must block uninstall");
      require(service, "session.stop");
      for (unsigned i = 0; i < 500 && service.snapshot().at("session").at("state") != "stopped";
           ++i)
        std::this_thread::sleep_for(5ms);
      gate.arm();
      auto queued = require(service, "modules.uninstall", {{"id", "lifecycle.module"}});
      auto job_id = queued.at("jobs").back().at("id").get<std::string>();
      gate.wait();
      check(!action(service, "modules.remove", {{"id", "lifecycle.module"}}).at("ok"),
            "Registration edits must be locked during uninstall");
      check(!action(service, "modules.rescan").at("ok"),
            "Rescan must not re-register a deleting module");
      check(!action(service, "modules.installPackage", {{"path", path_string(zip)}}).at("ok"),
            "Installs must not race uninstall");
      require(service, "profiles.create", {{"name", "During uninstall"}});
      require(service, "jobs.cancel", {{"id", job_id}});
      gate.release();
      check(finished(service, job_id).at("state") == "cancelled" && fs::exists(current_root) &&
                fs::exists(previous_root),
            "Precommit cancellation must preserve all installed files");
      gate.arm();
      queued = require(service, "jobs.retry", {{"id", job_id}});
      job_id = queued.at("jobs").back().at("id");
      gate.wait();
      write_document(previous_root / "owner.json", {{"owner", "changed after preflight"}});
      gate.release();
      check(finished(service, job_id).at("state") == "failed" &&
                fs::exists(current_root / "payload/module/module.json"),
            "Changed ownership must be rechecked before deleting any installation");
      write_document(previous_root / "owner.json", original_marker);
      interrupted = read_document(storage / "state.json");
      auto &job = interrupted["jobs"].back();
      job["state"] = "committing";
      job.erase("error");
      job.erase("endedAt");
    }
    // Simulate an interrupted deletion with one owned root already removed.
    fs::remove_all(previous_root);
    write_document(storage / "state.json", interrupted);
    {
      LauncherService service(storage, false, {}, {}, nullptr);
      auto state = service.snapshot();
      const auto old_id = state.at("jobs").back().at("id");
      check(state.at("jobs").back().at("state") == "interrupted",
            "Interrupted uninstall must remain retryable after restart");
      auto queued = require(service, "jobs.retry", {{"id", old_id}});
      check(finished(service, queued.at("jobs").back().at("id")).at("state") == "completed",
            "Retry must tolerate previously removed owned roots");
      state = service.snapshot();
      check(state.at("modules").empty() && state.at("games")[0].at("moduleId") == "" &&
                state.at("profiles").size() == 2,
            "Uninstall must unlink games and retain unrelated edits");
      check(!fs::exists(current_root) && fs::exists(game) && fs::exists(zip),
            "Owned roots must be removed while original games and packages survive");
      const auto external = root / "external";
      fs::create_directories(external);
      write_document(external / "module.json", {{"id", "external.module"}});
      require(service, "modules.import", {{"path", path_string(external)}});
      check(!action(service, "modules.uninstall", {{"id", "external.module"}}).at("ok"),
            "Unmanaged folders must never enter owned uninstall");
      require(service, "modules.remove", {{"id", "external.module"}});
      check(fs::exists(external / "module.json"), "Unregister must preserve external files");
    }
    fs::remove_all(root);
    std::cout << "Manifest aliases/contracts, rollback retention, ownership/content protection, "
                 "active-session locks, cancellation, concurrent edits and interrupted uninstall "
                 "retry passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << "\nFixtures retained at " << root << '\n';
    return 1;
  }
}
