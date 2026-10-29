#include "application/launcher_service.hpp"
#include "core/archive.hpp"
#include "core/releases.hpp"
#include "picosha2/picosha2.h"
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
    if (url.find("/releases/download/") != std::string::npos && pause.load()) {
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
    const auto id = "test.online.module", repo = "fixture/module";
    const auto asset = "module-" + host_key() + ".zip";
    const std::string base = "https://raw.githubusercontent.com/nimauria/Xenon-Modules/main/",
                      api = "https://api.github.com/repos/fixture/module/releases?per_page=30";
    write_document(root / "catalog.json",
                   {{"schema", "xenon.module-catalog"},
                    {"version", 1},
                    {"modules", Json::array({"modules/test.online.module.json"})}});
    write_document(
        root / "entry.json",
        {{"schema", "xenon.module-entry"},
         {"version", 1},
         {"id", id},
         {"name", "Fixture module"},
         {"type", "game"},
         {"description", "Explicit test-only catalogue entry"},
         {"publisher", {{"name", "Fixture publisher"}, {"verified", false}}},
         {"repository", {{"provider", "github"}, {"owner", "fixture"}, {"name", "module"}}},
         {"releaseAssets", {{host_key(), asset}}}});
    Json mapping = {{base + "catalog.json", {{"file", "catalog.json"}}},
                    {base + "modules/test.online.module.json", {{"file", "entry.json"}}},
                    {api, {{"file", "releases.json"}}}};
    const auto publish = [&](const std::string &version, bool digest_valid = true) {
      write_zip(root / "package.zip", {{"module/module.json", Json({{"id", id},
                                                                    {"name", "Fixture module"},
                                                                    {"version", version},
                                                                    {"settings", Json::array()}})
                                                                  .dump()},
                                       {"module/example.txt", "verified package"}});
      const auto url =
          "https://github.com/fixture/module/releases/download/v" + version + "/" + asset;
      write_document(
          root / "releases.json",
          Json::array({{{"tag_name", "v" + version},
                        {"name", nullptr},
                        {"body", nullptr},
                        {"prerelease", false},
                        {"draft", false},
                        {"assets",
                         Json::array({{{"name", asset},
                                       {"browser_download_url", url},
                                       {"size", fs::file_size(root / "package.zip")},
                                       {"digest",
                                        "sha256:" + (digest_valid ? hash_file(root / "package.zip")
                                                                  : std::string(64, '0'))}}})}}}));
      mapping[url] = {{"file", "package.zip"}};
      write_document(root / "responses.json", mapping);
    };
    publish("1.0.0");
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
    fs::remove_all(root);
    std::cout << "Registry, semver, digest verification, staged installation, network "
                 "cancellation/retry and offline cache passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
