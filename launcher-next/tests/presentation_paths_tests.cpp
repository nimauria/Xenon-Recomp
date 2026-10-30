#include "application/launcher_service.hpp"
#include "core/archive.hpp"
#include "core/launcher_paths.hpp"
#include <atomic>
#include <chrono>
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
Json request(LauncherService &service, const std::string &name, Json data = Json::object()) {
  return service.request({{"version", 1},
                          {"id", "test"},
                          {"command", "launcher.action"},
                          {"payload", {{"action", name}, {"data", data}}}});
}
Json action(LauncherService &service, const std::string &name, Json data = Json::object()) {
  const auto result = request(service, name, data);
  if (!result.at("ok"))
    throw std::runtime_error(result.at("error").at("message").get<std::string>());
  return result.at("data");
}
template <class Predicate> Json until(LauncherService &service, Predicate predicate) {
  for (unsigned i = 0; i < 1000; ++i) {
    auto state = service.snapshot();
    if (predicate(state))
      return state;
    std::this_thread::sleep_for(5ms);
  }
  throw std::runtime_error("Presentation/path fixture timed out");
}
Json finished(LauncherService &service, const Json &id) {
  const auto state = until(service, [&](const Json &state) {
    for (const auto &job : state.at("jobs"))
      if (job.at("id") == id)
        return job.at("state") != "queued" && job.at("state") != "running" &&
               job.at("state") != "committing" && job.at("state") != "cancelling";
    return false;
  });
  for (const auto &job : state.at("jobs"))
    if (job.at("id") == id)
      return job;
  throw std::runtime_error("Job missing");
}
class CatalogFixture final : public IHttpClient {
public:
  std::atomic_int generation = 1;
  std::atomic_bool pause = false, entered = false, fail = false;
  bool fixture() const override { return true; }
  HttpResponse get(const std::string &url, const HttpTransfer &transfer) override {
    if (fail.load())
      throw std::runtime_error("Explicit offline fixture");
    std::string body;
    if (url.ends_with("catalog.json"))
      body = Json({{"schema", "xenon.module-catalog"},
                   {"version", 1},
                   {"modules", Json::array({"modules/test.presentation.module.json"})}})
                 .dump();
    else if (url.ends_with("test.presentation.module.json")) {
      entered = true;
      unsigned attempts = 0;
      while (pause.load()) {
        if (transfer.cancelled && transfer.cancelled())
          throw OperationCancelled();
        if (++attempts > 5000)
          throw std::runtime_error("Fixture gate timeout");
        std::this_thread::sleep_for(1ms);
      }
      const auto suffix = std::to_string(generation.load());
      body = Json({{"schema", "xenon.module-entry"},
                   {"version", 1},
                   {"id", "test.presentation.module"},
                   {"name", "Presentation module"},
                   {"type", "game"},
                   {"description", "Catalogue description"},
                   {"publisher", {{"name", "Fixture"}, {"verified", false}}},
                   {"repository",
                    {{"provider", "github"}, {"owner", "fixture"}, {"name", "presentation"}}},
                   {"releaseAssets", {{"windows-x64", "fixture.zip"}}},
                   {"launcher",
                    {{"gameTitle", "Catalogue " + suffix},
                     {"description", "Catalogue description " + suffix},
                     {"tileArt", "assets/test.presentation.module/tile.png"},
                     {"heroArt", "assets/test.presentation.module/hero.png"},
                     {"tileArtFocalX", .2}}},
                   {"dlc", Json::array({{{"id", "extra-" + suffix},
                                         {"name", "Catalogue add-on " + suffix}}})}})
                 .dump();
    } else if (url.find("/assets/") != std::string::npos)
      body = "artwork fixture generation " + std::to_string(generation.load());
    else
      throw std::runtime_error("Unexpected fixture URL");
    transfer.write(body);
    return {200, "generation-" + std::to_string(generation.load()), body.size()};
  }
  void wait() {
    for (unsigned i = 0; i < 1000 && !entered.load(); ++i)
      std::this_thread::sleep_for(5ms);
    check(entered.load(), "Catalogue gate not reached");
  }
};
int main(int argc, char **argv) {
  const auto root = fs::temp_directory_path() / ("xenon-presentation-paths-" + make_id()),
             storage = root / "state";
  try {
    check(argc == 2, "Supply explicit runtime fixture host");
    fs::create_directories(storage);
    fs::create_directories(root / "module");
    std::ofstream(root / "Game.iso") << "game source";
    std::ofstream(root / "module/installed.png") << "installed artwork fixture";
    write_document(root / "module/module.json", {{"id", "test.presentation.module"},
                                                 {"name", "Presentation module"},
                                                 {"launcher",
                                                  {{"gameTitle", "Installed title"},
                                                   {"description", "Installed description"},
                                                   {"tileArt", "installed.png"}}}});
    auto http = std::make_shared<CatalogFixture>();
    std::string game_id;
    {
      LauncherService service(storage, false, fs::u8path(argv[1]), {}, http);
      auto state = service.snapshot();
      check(state.at("application").at("version").is_string() &&
                state.at("application").at("architecture") != "unknown",
            "Native build information must be explicit");
      check(state.at("captureFolderPath") == path_string(storage / "captures"),
            "Missing screenshot setting must use default");
      check(launcher_destination(storage, {{"paths/screenshots", ""}}, "paths/screenshots") ==
                storage / "captures",
            "Blank destinations must use defaults");
      auto old =
          launcher_locations(storage, {{"paths/games", "relative"},
                                       {"paths/profiles", path_string(root / "old profiles")}});
      check(old.at("destinations")[0].at("invalid") == true &&
                old.at("destinations")[0].at("effectivePath") == path_string(storage / "games"),
            "Invalid old paths must not resolve against the working directory");
      check(old.at("fixed")[1].at("path") == path_string(storage / "state.json") &&
                old.at("fixed")[1].at("legacyConfiguredPath") == path_string(root / "old profiles"),
            "Real profile store and retained legacy location must be distinct");
      check(!request(service, "settings.set", {{"key", "paths/games"}, {"value", "relative"}})
                 .at("ok"),
            "New relative paths must be rejected");
      check(!request(service, "settings.set",
                     {{"key", "paths/games"},
                      {"value", path_string(root) + std::string(1, '\0') + "hidden"}})
                 .at("ok"),
            "Embedded NUL paths must not reach native file operations");
      check(!request(service, "system.openPath", {{"target", "location"}, {"id", "../outside"}})
                 .at("ok"),
            "Location opening must accept stable IDs rather than arbitrary paths");
      check(!request(service, "settings.set",
                     {{"key", "paths/cache"}, {"value", path_string(root / "unused cache")}})
                 .at("ok"),
            "Missing cache destination folders must not silently change the active cache");
      action(service, "modules.import", {{"path", path_string(root / "module")}});
      action(service, "library.import", {{"paths", Json::array({path_string(root / "Game.iso")})}});
      game_id = service.snapshot().at("games")[0].at("id");
      action(service, "library.update",
             {{"id", game_id}, {"moduleId", "test.presentation.module"}, {"title", "My title"}});
      auto queued = action(service, "catalog.refresh");
      check(finished(service, queued.at("jobs").back().at("id")).at("state") == "completed",
            "Catalogue refresh must finish");
      state = service.snapshot();
      const auto &game = state.at("games")[0];
      check(game.at("title") == "My title" && game.at("description") == "Catalogue description 1",
            "Automatic presentation refresh must preserve edited titles");
      check(game.at("tileArt").get<std::string>().starts_with("xenon://app/catalog-tile/") &&
                service.asset_path("catalog-tile", "test.presentation.module").has_value(),
            "Library artwork must use the bounded native cache resource");
      check(state.at("dlcCatalog").at("test.presentation.module")[0].at("dlcId") == "extra-1",
            "Catalogue DLC must refresh with presentation");
      http->generation = 2;
      http->pause = true;
      http->entered = false;
      queued = action(service, "library.refreshMetadata", {{"id", game_id}});
      const auto refresh_id = queued.at("jobs").back().at("id");
      http->wait();
      check(service.snapshot().at("games")[0].at("title") == "Catalogue 1",
            "Explicit refresh must apply cached metadata immediately");
      action(service, "library.update", {{"id", game_id}, {"title", "Edited during refresh"}});
      action(service, "profiles.create", {{"name", "Concurrent profile"}});
      http->pause = false;
      check(finished(service, refresh_id).at("state") == "completed",
            "Queued metadata refresh must finish");
      state = service.snapshot();
      check(state.at("games")[0].at("title") == "Edited during refresh" &&
                state.at("games")[0].at("description") == "Catalogue description 2" &&
                state.at("profiles").size() == 2,
            "Catalogue completion must preserve concurrent user edits");
      const auto updated_artwork = service.asset_path("catalog-tile", "test.presentation.module");
      check(updated_artwork.has_value(), "Updated artwork must remain available");
      std::ifstream updated_image(*updated_artwork, std::ios::binary);
      const std::string image_bytes((std::istreambuf_iterator<char>(updated_image)), {});
      check(image_bytes == "artwork fixture generation 2",
            "Catalogue refresh must replace cached artwork bytes");
      updated_image.close();
      http->generation = 3;
      http->pause = true;
      http->entered = false;
      queued = action(service, "library.refreshMetadata", {{"id", game_id}});
      const auto third_id = queued.at("jobs").back().at("id");
      http->wait();
      auto duplicate = action(service, "library.refreshMetadata", {{"id", game_id}});
      check(duplicate.at("jobs").size() == queued.at("jobs").size(),
            "Repeated refresh must coalesce into the existing catalogue job");
      action(service, "library.update", {{"id", game_id}, {"moduleId", ""}});
      http->pause = false;
      check(finished(service, third_id).at("state") == "completed",
            "Refresh should survive unlinking");
      check(!service.snapshot().at("games")[0].contains("tileArt"),
            "Late catalogue results must not restore unlinked artwork");
      action(service, "library.update",
             {{"id", game_id}, {"moduleId", "test.presentation.module"}});
      http->fail = true;
      queued = action(service, "catalog.refresh");
      check(finished(service, queued.at("jobs").back().at("id")).at("state") == "failed",
            "Offline refresh must be reported");
      check(service.snapshot().at("catalog").at("entries").size() == 1 &&
                service.asset_path("catalog-tile", "test.presentation.module").has_value(),
            "Offline errors must preserve cached metadata and artwork");
      fs::create_directories(storage / "captures");
      std::ofstream(storage / "captures/Default.png") << "image";
      action(service, "settings.set", {{"key", "paths/screenshots"}, {"value", ""}});
      state = until(service, [](const Json &s) {
        return s.at("captureScan").at("state") == "ready" && s.at("captures").size() == 1;
      });
      check(state.at("captures")[0].at("name") == "Default.png" &&
                service.asset_path("captures", "Default.png").has_value(),
            "Blank capture path must use the same default for scanning and serving");
      fs::create_directories(root / "custom captures");
      std::ofstream(root / "custom captures/Custom.png") << "image";
      action(service, "settings.set",
             {{"key", "paths/screenshots"}, {"value", path_string(root / "custom captures")}});
      state = until(service, [](const Json &s) {
        return s.at("captureScan").at("state") == "ready" && s.at("captures").size() == 1 &&
               s.at("captures")[0].at("name") == "Custom.png";
      });
      action(service, "settings.resetKey", {{"key", "paths/screenshots"}});
      state = until(service, [](const Json &s) {
        return s.at("captureScan").at("state") == "ready" && s.at("captures").size() == 1 &&
               s.at("captures")[0].at("name") == "Default.png";
      });
      const auto profile_id = state.at("activeProfileId");
      check(!request(service, "profiles.update", {{"id", profile_id}, {"savePath", "relative"}})
                 .at("ok"),
            "Profile overrides must be absolute or blank");
      action(service, "profiles.update",
             {{"id", profile_id}, {"savePath", path_string(root / "profile saves")}});
      action(service, "session.launch", {{"gameId", game_id}});
      state =
          until(service, [](const Json &s) { return s.at("session").at("state") == "running"; });
      const auto config =
          read_document(storage / "sessions" /
                        fs::u8path(state.at("session").at("sessionId").get<std::string>()) /
                        "launch-config.json");
      check(config.at("gameRoot") == path_string(storage / "games") &&
                config.at("savePath") == path_string(root / "profile saves") &&
                config.at("screenshotsPath") == path_string(storage / "captures"),
            "Launch paths must agree with default destinations and profile overrides");
      action(service, "session.stop");
      until(service, [](const Json &s) { return s.at("session").at("state") == "stopped"; });
    }
    {
      LauncherService offline(storage, false, {}, {}, nullptr);
      const auto before = offline.snapshot().at("jobs").size();
      action(offline, "library.refreshMetadata", {{"id", game_id}});
      const auto state = offline.snapshot();
      check(state.at("jobs").size() == before &&
                state.at("games")[0].at("title") == "Catalogue 3" &&
                offline.asset_path("catalog-tile", "test.presentation.module").has_value(),
            "Offline restart must reuse cached presentation without claiming a network refresh");
    }
    fs::remove_all(root);
    std::cout << "Catalogue artwork/text/DLC refresh, coalescing, stale-result protection, offline "
                 "reuse, destination defaults/reset/scan/launch and profile overrides passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << "\nFixtures retained at " << root << '\n';
    return 1;
  }
}
