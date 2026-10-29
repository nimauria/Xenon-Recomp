#include "adapters/host_services.hpp"
#include "application/launcher_service.hpp"
#include "core/archive.hpp"
#include <fstream>
#include <iostream>
using namespace xenon::launcher_next;
namespace fs = std::filesystem;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(F operation, const char *message) {
  bool failed = false;
  try {
    operation();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed, message);
}
Json action(LauncherService &service, const std::string &command, Json data = Json::object()) {
  const auto reply = service.request({{"version", 1},
                                      {"id", "test"},
                                      {"command", "launcher.action"},
                                      {"payload", {{"action", command}, {"data", data}}}});
  if (!reply.at("ok"))
    throw std::runtime_error(reply.at("error").at("message").get<std::string>());
  return reply.at("data");
}
Json terminal(LauncherService &service, const std::string &id) {
  for (unsigned i = 0; i < 1000; ++i) {
    const auto state = service.snapshot();
    for (const auto &job : state.at("jobs"))
      if (job.at("id") == id) {
        const auto status = job.value("state", "");
        if (status == "completed" || status == "failed" || status == "cancelled")
          return job;
      }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("Service operation did not finish");
}
int main() {
  const auto root = fs::temp_directory_path() / ("xenon-services-" + make_id());
  auto cancel = std::make_shared<std::atomic_bool>(false);
  try {
    fs::create_directories(root / "content");
    std::ofstream(root / "content/hello.txt") << "fixture";
    const auto unavailable = unavailable_host_services();
    check(unavailable->snapshot()["capabilities"]["filesystem"] == false &&
              !unavailable->snapshot().at("fixture").get<bool>(),
          "Production must not invent runtime capabilities");
    rejects([&] { unavailable->execute("filesystem.mount", Json::object(), cancel); },
            "Unavailable services must reject mutation");
    {
      auto provider = fixture_host_services(root / "fixture");
      const auto execute = [&](const std::string &command, Json data = Json::object()) {
        return provider->execute(command, data, cancel);
      };
      execute("filesystem.mount", {{"mountPoint", "game:"},
                                   {"kind", "host"},
                                   {"source", path_string(root / "content")},
                                   {"readOnly", true}});
      check(execute("filesystem.test", {{"path", "game:/hello.txt"}}).at("exists"),
            "Host-folder fixture must test the actual registered source");
      execute("filesystem.link", {{"alias", "dvd:"}, {"target", "game:/"}});
      check(execute("filesystem.test", {{"path", "dvd:/hello.txt"}}).at("exists"),
            "Guest aliases must resolve through the mount table");
      rejects([&] { execute("filesystem.test", {{"path", "game:/../outside.txt"}}); },
              "Guest traversal must be rejected");
      rejects(
          [&] {
            execute("filesystem.mount", {{"mountPoint", "GAME:"},
                                         {"kind", "host"},
                                         {"source", path_string(root / "content")}});
          },
          "Guest mount identity must be case-insensitive");
      execute("filesystem.link", {{"alias", "one:"}, {"target", "two:"}});
      execute("filesystem.link", {{"alias", "two:"}, {"target", "one:"}});
      rejects([&] { execute("filesystem.test", {{"path", "one:"}}); },
              "Symbolic-link cycles must be rejected");
      execute("filesystem.cwd", {{"path", "game:/"}});
      execute("filesystem.unlink", {{"alias", "dvd:"}});
      execute("input.assign", {{"user", 0}, {"deviceId", "fixture-keyboard"}});
      rejects([&] { execute("input.vibrate", {{"user", 0}}); },
              "Vibration requires a gamepad route");
      execute("input.sourceAdd", {{"user", 0}, {"deviceId", "fixture-pad"}});
      check(execute("input.vibrate", {{"user", 0}}).at("fixture"),
            "A fixture must never claim to drive physical vibration");
      execute("input.profileBind", {{"user", 0}, {"profileId", "fixture-standard"}});
      check(provider->snapshot()["input"]["users"][0]["sources"].size() == 2,
            "Multi-source routes must retain both sources");
      rejects(
          [&] { execute("input.assign", {{"user", 4294967296ull}, {"deviceId", "fixture-pad"}}); },
          "Overflowing player indices must not wrap");
      rejects([&] { execute("input.profileBind", {{"user", 0}, {"profileId", "missing"}}); },
              "Unknown bindings profiles must be rejected");
      execute("network.fixtureScenario", {{"scenario", "transport-only"}});
      check(provider->snapshot()["network"]["serviceReachable"] == true &&
                provider->snapshot()["network"]["onlineServicesReady"] == false,
            "Transport alone must not imply online-service readiness");
      execute("network.fixtureScenario", {{"scenario", "ready"}});
      check(provider->snapshot()["network"]["onlineServicesReady"] == true,
            "Explicit ready fixture must project negotiated readiness");
      execute("network.fixtureScenario", {{"scenario", "protocol-mismatch"}});
      check(provider->snapshot()["network"]["protocolCompatible"] == false,
            "Protocol failure must revoke readiness");
      const auto identified =
          execute("content.identify", {{"path", path_string(root / "content")}});
      check(identified["fixture"] == true && identified["integrityVerified"] == false,
            "Fixture identification must not assert game integrity");
      const auto revision = provider->snapshot().at("revision");
      cancel->store(true);
      rejects([&] { execute("input.clear", {{"user", 0}}); },
              "Cancelled operations must not apply");
      cancel->store(false);
      check(provider->snapshot().at("revision") == revision,
            "Cancellation must preserve the provider revision");
    }
    {
      auto provider = fixture_host_services(root / "fixture");
      check(provider->snapshot()["input"]["users"][0]["profileId"] == "fixture-standard",
            "Bindings must persist through provider reconstruction");
    }
    {
      LauncherService service(root / "service", false, root / "explicit-unused-fixture-host", {},
                              nullptr);
      const auto state = action(service, "filesystem.mount",
                                {{"mountPoint", "data:"},
                                 {"kind", "host"},
                                 {"source", path_string(root / "missing")},
                                 {"readOnly", true}});
      const auto failed = terminal(service, state["jobs"].back().at("id"));
      check(failed.at("state") == "failed", "Invalid service work must fail asynchronously");
      fs::create_directories(root / "missing");
      const auto retried = action(service, "jobs.retry", {{"id", failed.at("id")}});
      check(terminal(service, retried["jobs"].back().at("id")).at("state") == "completed",
            "A repaired service request must support explicit retry");
      check(service.snapshot()["runtimeServices"]["filesystem"]["mounts"].size() == 1,
            "The service snapshot must expose actual adapter state");
    }
    {
      const auto modules = root / "installed-modules";
      fs::create_directories(modules / "example");
      Json definition = {
          {"id", "scan.example"},
          {"name", "Scan example"},
          {"version", "1"},
          {"settings", Json::array({Json{{"id", "flag"}, {"type", "bool"}, {"default", true}}})}};
      write_document(modules / "example/module.json", definition);
      LauncherService *service_ptr = nullptr;
      bool change_root = false;
      LauncherService service(
          root / "local-service", false, {},
          [&](const std::string &, const std::string &phase) {
            if (change_root && phase == "Module scan ready")
              action(*service_ptr, "settings.set",
                     {{"key", "paths/modules"}, {"value", path_string(root / "another-root")}});
          },
          nullptr);
      service_ptr = &service;
      action(service, "settings.set", {{"key", "paths/modules"}, {"value", path_string(modules)}});
      auto scan = action(service, "modules.rescan").at("jobs").back().at("id");
      check(terminal(service, scan).at("state") == "completed" &&
                service.snapshot().at("modules").size() == 1,
            "Local module scan must work without a Xenon host provider");
      action(service, "modules.enable", {{"id", "scan.example"}, {"enabled", false}});
      action(service, "modules.setting",
             {{"id", "scan.example"}, {"key", "flag"}, {"value", false}});
      definition["version"] = "2";
      definition["name"] = "Updated name";
      write_document(modules / "example/module.json", definition);
      scan = action(service, "modules.rescan").at("jobs").back().at("id");
      check(terminal(service, scan).at("state") == "completed", "Manifest rescan should complete");
      auto module = service.snapshot().at("modules").front();
      check(module.at("version") == "2" && module.at("enabled") == false &&
                module.at("settings").at("flag") == false,
            "Rescan must refresh metadata while preserving disabled state and settings");
      std::ofstream(modules / "example/module.json") << "{broken";
      scan = action(service, "modules.rescan").at("jobs").back().at("id");
      check(terminal(service, scan).contains("warning") &&
                service.snapshot().at("modules").front().at("version") == "2",
            "Corrupt source manifests must not replace the last saved registration");
      write_document(modules / "example/module.json", definition);
      change_root = true;
      scan = action(service, "modules.rescan").at("jobs").back().at("id");
      check(terminal(service, scan).at("state") == "failed" &&
                service.snapshot().at("settings").at("paths/modules") ==
                    path_string(root / "another-root"),
            "Changing the module root during a scan must invalidate its result and preserve the "
            "new preference");
    }
    fs::remove_all(root);
    std::cout
        << "Unavailable/fixture service contracts, VFS boundaries/links, player routing/profiles, "
           "network readiness, cancellation, persistence and queued repair/retry passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
