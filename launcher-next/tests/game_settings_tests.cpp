#include "application/launcher_service.hpp"
#include "core/game_settings.hpp"
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
  auto reply = request(service, name, std::move(data));
  if (!reply.at("ok"))
    throw std::runtime_error(reply.at("error").at("message").get<std::string>());
  return reply.at("data");
}
template <class F> void rejects(F operation, const char *message) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::exception &) {
    rejected = true;
  }
  check(rejected, message);
}
void terminal(LauncherService &service) {
  for (unsigned i = 0; i < 500; ++i) {
    auto state = service.snapshot().at("session").value("state", "");
    if (state == "stopped" || state == "failed")
      return;
    std::this_thread::sleep_for(10ms);
  }
  throw std::runtime_error("Session fixture did not stop");
}
int main(int argc, char **argv) {
  const auto root = fs::temp_directory_path() / ("xenon-game-settings-" + make_id());
  try {
    check(argc == 2, "Supply fixture host");
    const Json global = {{"audio/masterVolume", .8}, {"runtime/graphicsBackend", "Vulkan"}};
    const Json profile = {{"isolatedSettings", true},
                          {"runtimeOverrides", {{"audio/masterVolume", .5}}}};
    check(effective_game_settings({{"runtimeOverrides", {{"audio/masterVolume", .2}}}}, profile,
                                  global)
                  .at("audio/masterVolume") == .2,
          "Game must override profile and launcher");
    check(effective_game_settings(Json::object(), profile, global).at("audio/masterVolume") == .5,
          "Reset game must inherit profile");
    rejects([&] { game_runtime_values({{"audio/masterVolume", 1.1}}); }, "Reject invalid ranges");
    rejects([&] { game_runtime_values({{"runtime/graphicsBackend", "fake"}}); },
            "Reject unsupported enum");
    rejects([&] { game_runtime_values({{"community/discordRichPresence", true}}); },
            "No arbitrary global settings");
    fs::create_directories(root / "module");
    std::ofstream(root / "One.xex") << "one";
    std::ofstream(root / "Two.xex") << "two";
    write_document(root / "module/module.json",
                   {{"id", "test.game.settings"},
                    {"name", "Settings fixture"},
                    {"settings", Json::array({{{"id", "difficulty"},
                                               {"type", "choice"},
                                               {"default", "normal"},
                                               {"options", Json::array({"normal", "hard"})}},
                                              {{"id", "fov"},
                                               {"type", "number"},
                                               {"default", 75},
                                               {"minimum", 60},
                                               {"maximum", 120}}})}});
    std::string first, second;
    {
      LauncherService service(root / "data", false, fs::u8path(argv[1]), {}, nullptr);
      action(service, "application.ready");
      action(service, "modules.import", {{"path", path_string(root / "module")}});
      auto state = action(
          service, "library.import",
          {{"paths", Json::array({path_string(root / "One.xex"), path_string(root / "Two.xex")})}});
      first = state.at("games")[0].at("id");
      second = state.at("games")[1].at("id");
      for (const auto &id : {first, second})
        action(service, "library.update", {{"id", id}, {"moduleId", "test.game.settings"}});
      action(service, "settings.set", {{"key", "audio/masterVolume"}, {"value", .8}});
      state = action(service, "profiles.create",
                     {{"name", "Isolated"},
                      {"isolatedSettings", true},
                      {"runtimeOverrides", {{"audio/masterVolume", .5}}}});
      action(service, "profiles.activate", {{"id", state.at("profiles").back().at("id")}});
      const auto save = [&](const std::string &id, Json runtime, Json options) {
        return action(service, "library.settings.save",
                      {{"id", id},
                       {"moduleId", "test.game.settings"},
                       {"runtimeOverrides", runtime},
                       {"moduleOverrides", options}});
      };
      state = save(first, {{"audio/masterVolume", .2}, {"input/deadzone", .15}},
                   {{"difficulty", "hard"}, {"fov", 100}});
      check(!state.at("games")[1].contains("runtimeOverrides") &&
                state.at("modules")[0].at("settings").at("difficulty") == "normal",
            "Other game and module defaults untouched");
      const auto before = read_document(root / "data/state.json");
      check(!request(service, "library.settings.save",
                     {{"id", first}, {"moduleId", "wrong"}, {"runtimeOverrides", Json::object()}})
                 .at("ok"),
            "Stale module identity rejected");
      check(!request(service, "library.settings.save",
                     {{"id", first},
                      {"moduleId", "test.game.settings"},
                      {"runtimeOverrides", {{"audio/masterVolume", .3}}},
                      {"moduleOverrides", {{"difficulty", "impossible"}}}})
                 .at("ok"),
            "Invalid module option rejected");
      check(read_document(root / "data/state.json") == before, "Failed saves atomic");
      action(service, "session.launch", {{"gameId", first}});
      auto active = read_document(root / "data/active-session.json");
      const auto config = read_document(fs::u8path(active.at("sessionDir").get<std::string>()) /
                                        "launch-config.json");
      check(config.at("audioMasterVolume") == .2 && config.at("inputDeadzone") == .15 &&
                config.at("moduleSettings").at("difficulty") == "hard" &&
                config.at("moduleSettings").at("fov") == 100,
            "Actual launch configuration uses per-game settings");
      check(!request(service, "library.settings.save",
                     {{"id", first},
                      {"moduleId", "test.game.settings"},
                      {"runtimeOverrides", Json::object()}})
                 .at("ok"),
            "Cannot edit active game settings");
      save(second, {{"audio/masterVolume", .4}}, Json::object());
      action(service, "session.stop");
      terminal(service);
      save(first, Json::object(), Json::object());
      action(service, "session.launch", {{"gameId", first}});
      active = read_document(root / "data/active-session.json");
      const auto reset = read_document(fs::u8path(active.at("sessionDir").get<std::string>()) /
                                       "launch-config.json");
      check(reset.at("audioMasterVolume") == .5 &&
                reset.at("moduleSettings").at("difficulty") == "normal",
            "Reset restores inherited profile/module values");
      action(service, "session.stop");
      terminal(service);
    }
    {
      LauncherService service(root / "data", false, {}, {}, nullptr);
      const auto state = service.snapshot();
      check(state.at("games")[1].at("runtimeOverrides").at("audio/masterVolume") == .4,
            "Other game's settings survive restart");
    }
    fs::remove_all(root);
    std::cout << "Per-game inheritance, isolation, persistence, validation, launch linkage and "
                 "active-session locks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
