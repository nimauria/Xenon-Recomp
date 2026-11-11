#include "application/launcher_service.hpp"
#include "core/archive.hpp"
#include "core/releases.hpp"
#include "picosha2/picosha2.h"
#include "repository_fixture.hpp"
#include <chrono>
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
Json action(LauncherService &service, const std::string &action, Json data = Json::object()) {
  auto result = service.request({{"id", "test"},
                                 {"version", 1},
                                 {"command", "launcher.action"},
                                 {"payload", {{"action", action}, {"data", data}}}});
  if (!result.at("ok"))
    throw std::runtime_error(result["error"]["message"].get<std::string>());
  return result.at("data");
}
Json finished(LauncherService &service, const std::string &id) {
  for (unsigned i = 0; i < 1000; ++i) {
    const auto snapshot = service.snapshot();
    for (const auto &job : snapshot.at("jobs"))
      if (job.at("id") == id) {
        const auto state = job.at("state").get<std::string>();
        if (state != "queued" && state != "running" && state != "cancelling" &&
            state != "committing" && !job.value("cleaningUp", false))
          return job;
      }
    std::this_thread::sleep_for(5ms);
  }
  throw std::runtime_error("Online job timed out");
}
Json run(LauncherService &service, const std::string &name, Json data = Json::object()) {
  const auto queued = action(service, name, data);
  return finished(service, queued["jobs"].back().at("id"));
}
std::string hash_file(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(stream)), {});
  return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}
class GatedHttp final : public IHttpClient {
  std::shared_ptr<IHttpClient> delegate_;

public:
  std::atomic_bool pause = false, entered = false;
  explicit GatedHttp(const fs::path &root) : delegate_(fixture_http(root)) {}
  bool fixture() const override { return true; }
  HttpResponse get(const std::string &url, const HttpTransfer &transfer) override {
    if (url.ends_with("/xenon-module.json") && pause.load()) {
      entered = true;
      while (pause.load()) {
        if (transfer.cancelled && transfer.cancelled())
          throw OperationCancelled();
        std::this_thread::sleep_for(1ms);
      }
    }
    return delegate_->get(url, transfer);
  }
};
int main(int argc, char **argv) {
  const auto root = fs::temp_directory_path() / ("xenon-online-" + make_id());
  try {
    check(argc == 2, "Supply the settings schema fixture");
    fs::create_directories(root / "state");
    fs::copy_file(fs::u8path(argv[1]), root / "state/settings-schema.json");
    const auto bundled = fs::u8path(argv[1]).parent_path() / "module-catalog-fallback.json";
    fs::create_directories(root / "bundled");
    fs::copy_file(bundled, root / "bundled/module-catalog-fallback.json");
    {
      LauncherService offline(root / "bundled", false);
      check(offline.snapshot()["catalog"]["state"] == "bundled" &&
                offline.snapshot()["catalog"]["entries"][0]["sourcePath"] ==
                    "games/nimauria/project-gracemeria",
            "Version-2 bundled catalogue must load without a network connection");
    }
    const auto id = "test.online.module";
    const std::string base = "https://raw.githubusercontent.com/nimauria/Xenon-Modules/main/";
    write_document(root / "catalog.json",
                   {{"schema", "xenon.module-catalog"},
                    {"version", 2},
                    {"modules", Json::array({"games/fixture/module/entry.json"})}});
    write_document(
        root / "entry.json",
        {{"schema", "xenon.module-entry"},
         {"version", 2},
         {"id", id},
         {"name", "Fixture module"},
         {"type", "game"},
         {"description", "Explicit test-only catalogue entry"},
         {"publisher", {{"name", "Fixture publisher"}, {"verified", false}}},
         {"repository", {{"provider", "github"}, {"owner", "nimauria"}, {"name", "Xenon-Modules"}}},
         {"sourcePath", "games/fixture/module"},
         {"platforms", Json::array({host_key()})}});
    Json mapping = {{base + "catalog.json", {{"file", "catalog.json"}}},
                    {base + "games/fixture/module/entry.json", {{"file", "entry.json"}}}};
    const auto publish = [&](const std::string &version, bool digest_valid = true,
                             const std::string &content = "verified package") {
      repository_fixture(root, mapping, base + "games/fixture/module", id, version, digest_valid,
                         content);
    };
    publish("1.0.0");
    const auto entry =
        catalog_entry(read_document(root / "entry.json"), "games/fixture/module/entry.json");
    const auto expect_rejected = [&](const std::function<void()> &operation) {
      bool rejected = false;
      try {
        operation();
      } catch (const std::exception &) {
        rejected = true;
      }
      check(rejected, "Unsafe repository metadata must be rejected");
    };
    auto invalid_package = read_document(root / "repository-package.json");
    invalid_package["files"][0]["path"] = "../outside.dll";
    expect_rejected([&] { repository_package(invalid_package, entry); });
    invalid_package = read_document(root / "repository-package.json");
    invalid_package["files"].push_back(invalid_package["files"][0]);
    expect_rejected([&] { repository_package(invalid_package, entry); });
    invalid_package = read_document(root / "repository-package.json");
    invalid_package["moduleId"] = "another.contributor.game";
    expect_rejected([&] { repository_package(invalid_package, entry); });
    auto invalid_entry = read_document(root / "entry.json");
    invalid_entry["sourcePath"] = "games/fixture/../module";
    expect_rejected([&] { catalog_entry(invalid_entry, "games/fixture/../module/entry.json"); });
    invalid_entry = read_document(root / "entry.json");
    invalid_entry["repository"]["name"] = "Other-Repository";
    expect_rejected([&] { catalog_entry(invalid_entry, "games/fixture/module/entry.json"); });
    auto invalid_catalog = read_document(root / "catalog.json");
    invalid_catalog["modules"].push_back(invalid_catalog["modules"][0]);
    expect_rejected([&] { catalog_paths(invalid_catalog); });
    check(Version::parse("v1.10.0")->compare(*Version::parse("1.9.9")) > 0,
          "Semantic version sorting must not be lexical");
    check(Version::parse("1.0.0-rc.10")->compare(*Version::parse("1.0.0-rc.2")) > 0,
          "Prerelease numeric identifiers must compare numerically");
    check(!Version::parse("1.0.0-rc.") && !Version::parse("01.0.0"),
          "Malformed semantic versions must be rejected");
    check(!allowed_update_url("https://github.com@evil.example/file") &&
              !allowed_update_url("http://github.com/file"),
          "Update hosts and TLS scheme must be constrained");
    const std::string abc = "abc";
    check(picosha2::hash256_hex_string(abc.begin(), abc.end()) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 must match the standard known vector");
    auto http = std::make_shared<GatedHttp>(root);
    {
      LauncherService service(root / "state", false, {}, {}, http);
      check(run(service, "catalog.refresh")["state"] == "completed",
            "Catalogue should refresh through the transport");
      check(service.snapshot()["catalog"]["entries"][0]["id"] == id,
            "Registry identity should be retained");
      check(run(service, "modules.checkUpdate", {{"id", id}})["state"] == "completed",
            "Release check should complete");
      check(service.snapshot()["moduleUpdates"][id]["canDownload"],
            "Valid release digest should enable downloading");
      check(run(service, "modules.downloadUpdate", {{"id", id}})["state"] == "completed",
            "Package should download and verify");
      auto snapshot = service.snapshot();
      check(snapshot["modules"].empty() && snapshot["moduleUpdates"][id]["canInstall"],
            "Download must stage, not silently install");
      check(!snapshot["moduleUpdates"][id].contains("_packagePath"),
            "Staging paths must stay native");
      const auto cached =
          fs::u8path(read_document(root / "state/state.json")["moduleUpdates"][id]["_packagePath"]
                         .get<std::string>());
      std::ofstream(cached, std::ios::trunc) << "changed after download";
      check(run(service, "modules.installDownloaded", {{"id", id}})["state"] == "failed" &&
                service.snapshot()["modules"].empty(),
            "Install must recheck the staged digest before extraction");
      check(run(service, "modules.downloadUpdate", {{"id", id}})["state"] == "completed",
            "Re-download should repair staged data");
      check(run(service, "modules.installDownloaded", {{"id", id}})["state"] == "completed",
            "Verified module should install");
      check(service.snapshot()["modules"][0]["version"] == "1.0.0" &&
                !service.snapshot()["moduleUpdates"][id]["canInstall"],
            "Installed release should be current");
      publish("2.0.0", false);
      run(service, "modules.checkUpdate", {{"id", id}});
      check(run(service, "modules.downloadUpdate", {{"id", id}})["state"] == "failed" &&
                service.snapshot()["modules"][0]["version"] == "1.0.0",
            "Digest mismatch must preserve the previous installation");
      publish("2.0.0");
      run(service, "modules.checkUpdate", {{"id", id}});
      http->pause = true;
      http->entered = false;
      auto queued = action(service, "modules.downloadUpdate", {{"id", id}});
      const auto job_id = queued["jobs"].back()["id"].get<std::string>();
      for (unsigned i = 0; i < 2000 && !http->entered.load(); ++i)
        std::this_thread::sleep_for(1ms);
      check(http->entered, "Download should reach the controlled network wait");
      action(service, "jobs.cancel", {{"id", job_id}});
      check(finished(service, job_id)["state"] == "cancelled",
            "Cancellation must interrupt a pending network transfer");
      http->pause = false;
      auto retry = action(service, "jobs.retry", {{"id", job_id}});
      check(finished(service, retry["jobs"].back()["id"])["state"] == "completed",
            "Network retry should use the checked release");
      check(run(service, "modules.installDownloaded", {{"id", id}})["state"] == "completed",
            "New checked release should install");
      check(service.snapshot()["modules"][0]["version"] == "2.0.0",
            "Update version must match the package");
      action(service, "modules.rollback", {{"id", id}});
      check(service.snapshot()["modules"][0]["version"] == "1.0.0",
            "Online updates must retain rollback");
      mapping[base + "catalog.json"]["status"] = 503;
      write_document(root / "responses.json", mapping);
      check(run(service, "catalog.refresh")["state"] == "failed",
            "Offline refresh must report failure");
      check(service.snapshot()["catalog"]["entries"].size() == 1 &&
                service.snapshot()["catalog"]["state"] == "offline",
            "Offline operation must retain the last catalogue");
      action(service, "updates.clearHistory", {{"id", id}});
      check(service.snapshot()["updateHistory"].empty(), "Per-module update history should clear");
    }
    // Same-version source edits must be visible, including after reopening and rollback.
    mapping[base + "catalog.json"].erase("status");
    publish("1.0.0", true, "repository edit without a version bump");
    {
      LauncherService service(root / "state", false, {}, {}, http);
      check(run(service, "modules.checkUpdate", {{"id", id}})["state"] == "completed" &&
                service.snapshot()["moduleUpdates"][id]["canDownload"],
            "Same-version repository edits must be downloadable after reopening");
      check(run(service, "modules.downloadUpdate", {{"id", id}})["state"] == "completed" &&
                run(service, "modules.installDownloaded", {{"id", id}})["state"] == "completed",
            "Same-version repository edit must install safely");
      run(service, "modules.checkUpdate", {{"id", id}});
      check(!service.snapshot()["moduleUpdates"][id]["canDownload"],
            "Installed source digest must suppress duplicate updates");
    }
    fs::remove_all(root);
    std::cout << "Registry, semver, digest verification, staged installation, network "
                 "cancellation/retry and offline cache passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
