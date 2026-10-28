#include "application/launcher_service.hpp"
#include "core/archive.hpp"
#include <fstream>
#include <iostream>
#include <random>
using namespace xenon::launcher_next;
void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
Json call(LauncherService &service, const std::string &command, Json payload = Json::object()) {
  return service.request(
      {{"id", "test"}, {"version", 1}, {"command", command}, {"payload", payload}});
}
int main() {
  const auto folder = std::filesystem::temp_directory_path() /
                      ("xenon-launcher-tests-" + std::to_string(std::random_device{}()));
  try {
    std::filesystem::create_directories(folder);
    {
      LauncherService service(folder);
      check(service.snapshot()["games"].empty(), "Production must start empty");
      check(!service.request(Json::array()).at("ok"), "Malformed request should fail safely");
      check(!call(service, "profiles.remove", {{"id", "local"}}).at("ok"),
            "Active profile must be protected");
      check(!call(service, "profiles.activate", {{"id", "missing"}}).at("ok"),
            "Missing profile activation must fail");
      const auto result =
          call(service, "profiles.create",
               {{"name", "Pilot"}, {"description", "Saved description"}, {"region", "Europe"}});
      check(result.at("ok"), "Profile creation should succeed");
      const auto profile = result.at("data").at("profiles").back();
      check(profile.at("description") == "Saved description", "Profile metadata must persist");
      check(!call(service, "profiles.create", {{"name", "pilot"}}).at("ok"),
            "Duplicate names should fail");
      check(call(service, "profiles.activate", {{"id", profile.at("id")}}).at("ok"),
            "Profile activation should succeed");
      check(call(service, "profiles.remove", {{"id", "local"}}).at("ok"),
            "Inactive removal should succeed");
      const auto profile_file = folder / "export.xenonprofile";
      check(call(service, "launcher.action",
                 {{"action", "profiles.export"},
                  {"data", {{"id", profile.at("id")}, {"path", path_string(profile_file)}}}})
                .at("ok"),
            "Profile export should succeed");
      Json exported;
      std::ifstream(profile_file) >> exported;
      check(exported.at("schema") == "xenon.launcher.profile" &&
                !exported["profile"].contains("id") && !exported["profile"].contains("gamePath"),
            "Portable profile export must exclude machine identity/paths");
      check(call(service, "launcher.action",
                 {{"action", "profiles.import"}, {"data", {{"path", path_string(profile_file)}}}})
                .at("ok"),
            "Legacy-compatible profile import should succeed");
      const auto package = folder / "module.zip";
      write_zip(
          package,
          {{"module/module.json",
            R"({"id":"test.module","name":"Test module","version":"1","settings":[],"launcher":{"tileArt":"images/cover.PNG","heroArt":"images/cover.PNG","tileArtFocalX":0.25}})"},
           {"module/test.txt", "original"},
           {"module/images/cover.PNG", "test-image"}});
      check(call(service, "launcher.action",
                 {{"action", "modules.installPackage"}, {"data", {{"path", path_string(package)}}}})
                .at("ok"),
            "Valid module ZIP should install");
      const auto content = folder / "content";
      std::filesystem::create_directories(content);
      check(call(service, "launcher.action",
                 {{"action", "library.import"},
                  {"data", {{"paths", Json::array({path_string(content)})}}}})
                .at("ok"),
            "Source registration should succeed");
      const auto gameId = service.snapshot()["games"][0].at("id").get<std::string>();
      check(call(service, "launcher.action",
                 {{"action", "library.update"},
                  {"data", {{"id", gameId}, {"moduleId", "test.module"}}}})
                .at("ok"),
            "Module association should succeed");
      check(service.snapshot()["games"][0].contains("tileArt") &&
                service.asset_path("game-tile", gameId).has_value(),
            "Nested artwork inside module should resolve through its game ID");
      write_zip(
          package,
          {{"module/module.json",
            R"({"id":"test.module","name":"Test module","version":"2","settings":[],"launcher":{"tileArt":"../../outside.png"}})"},
           {"module/test.txt", "updated"}});
      check(call(service, "launcher.action",
                 {{"action", "modules.installPackage"}, {"data", {{"path", path_string(package)}}}})
                .at("ok"),
            "New module version should stage and install");
      check(service.snapshot()["modules"][0]["version"] == "2", "Installed version should update");
      std::ofstream(folder / "outside.png") << "outside";
      check(!service.asset_path("game-tile", gameId).has_value(),
            "Module artwork must not escape its installed root");
      check(call(service, "launcher.action",
                 {{"action", "modules.rollback"}, {"data", {{"id", "test.module"}}}})
                .at("ok"),
            "Rollback should restore previous package");
      check(service.snapshot()["modules"][0]["version"] == "1", "Rollback version must match");
      check(service.asset_path("game-tile", gameId).has_value(),
            "Rollback should restore game artwork");
      check(call(service, "launcher.action",
                 {{"action", "library.update"}, {"data", {{"id", gameId}, {"moduleId", ""}}}})
                .at("ok"),
            "Empty module selection should unlink the game");
      check(!service.snapshot()["games"][0].contains("tileArt") &&
                !service.snapshot()["games"][0].contains("moduleId"),
            "Unlinked games must not retain old module artwork");
      check(
          call(service, "launcher.action", {{"action", "support.bundle"}, {"data", Json::object()}})
              .at("ok"),
          "Support ZIP should be created");
      bool support = false;
      for (const auto &entry : std::filesystem::recursive_directory_iterator(folder / "support"))
        if (entry.path().extension() == ".zip") {
          std::ifstream input(entry.path(), std::ios::binary);
          char magic[2];
          input.read(magic, 2);
          support = magic[0] == 'P' && magic[1] == 'K';
        }
      check(support, "Support artifact must be a real ZIP");
      check(!call(service, "session.launch", {{"gameId", "missing"}}).at("ok"),
            "Runtime must not be fabricated");
      std::ofstream(folder / "not-an-image.txt") << "preserve";
      check(!call(service, "launcher.action",
                  {{"action", "captures.delete"}, {"data", {{"id", "../not-an-image.txt"}}}})
                 .at("ok"),
            "Capture traversal must fail");
    }
    {
      LauncherService restored(folder);
      check(restored.snapshot()["profiles"].size() == 2, "State must survive restart");
      check(restored.snapshot()["profiles"][0]["name"] == "Pilot", "Profile should be restored");
      std::ofstream(folder / "state.json", std::ios::trunc) << "invalid";
      bool rejected = false;
      try {
        LauncherService corrupt(folder);
      } catch (...) {
        rejected = true;
      }
      check(rejected, "Corrupt state must not be silently reset");
      std::ifstream preserved(folder / "state.json");
      std::string text;
      preserved >> text;
      check(text == "invalid", "Corrupt source data must remain intact");
    }
    std::filesystem::remove_all(folder);
    std::cout << "Launcher request, identity, persistence and failure checks "
                 "passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
