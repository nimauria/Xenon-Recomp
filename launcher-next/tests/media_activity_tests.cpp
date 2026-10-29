#include "application/launcher_service.hpp"
#include "core/game_media.hpp"
#include "core/game_activity.hpp"
#include <fstream>
#include <iostream>
using namespace xenon::launcher_next;
namespace fs = std::filesystem;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(F work, const char *message) {
  bool rejected = false;
  try {
    work();
  } catch (const std::exception &) {
    rejected = true;
  }
  check(rejected, message);
}
Json request(LauncherService &service, const std::string &command, Json data = Json::object()) {
  return service.request({{"version", 1},
                          {"id", "test"},
                          {"command", "launcher.action"},
                          {"payload", {{"action", command}, {"data", data}}}});
}
Json action(LauncherService &service, const std::string &command, Json data = Json::object()) {
  const auto result = request(service, command, data);
  if (!result.at("ok"))
    throw std::runtime_error(result.at("error").at("message").get<std::string>());
  return result.at("data");
}
Json wait(LauncherService &service, const std::function<bool(const Json &)> &predicate) {
  for (unsigned i = 0; i < 1500; ++i) {
    const auto state = service.snapshot();
    if (predicate(state))
      return state;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("Media/activity operation timed out");
}
Json terminal(LauncherService &service, const Json &id) {
  return wait(service, [&](const auto &s) {
    for (const auto &job : s.at("jobs"))
      if (job.at("id") == id)
        return job.value("state", "") == "completed" || job.value("state", "") == "failed";
    return false;
  });
}
void pure_activity_tests() {
  Json game = {{"id", "g"},
               {"contentPath", "/fictional/disc.iso"},
               {"lastPlayed", "2025-01-01T00:00:00Z"},
               {"legacyMetadata", {{"playCount", 4}, {"totalPlayTimeMs", 12000}}}};
  normalize_game_activity(game);
  check(game.at("playCount") == 4 && game.at("totalPlayTimeMs") == 12000,
        "Legacy totals must be projected without losing their source metadata");
  Json state = {{"games", Json::array({game})}, {"history", Json::array()}};
  Json report = {{"sessionId", "s1"},
                 {"gameId", "g"},
                 {"state", "running"},
                 {"fixture", true},
                 {"runningAtEpochMs", 1000}};
  check(apply_session_activity(state, report), "Running transition must be recorded");
  check(!apply_session_activity(state, report),
        "Repeated running reports must not count another launch");
  auto restored = Json::parse(state.dump());
  report["state"] = "stopped";
  report["endedAtEpochMs"] = 2500;
  apply_session_activity(restored, report);
  check(restored["games"][0]["fixtureActivity"]["playCount"] == 1 &&
            restored["games"][0]["fixtureActivity"]["totalPlayTimeMs"] == 1500,
        "Reattachment must count one complete test session");
  check(restored["games"][0]["playCount"] == 4 && restored["games"][0]["totalPlayTimeMs"] == 12000,
        "Test activity must not change real game statistics");
  restored["history"] = Json::array();
  check(!apply_session_activity(restored, report) && restored["history"].empty(),
        "Cleared history must not be repopulated by the current terminal session");
  report["sessionId"] = "s2";
  report["fixture"] = false;
  report["runningAtEpochMs"] = 5000;
  report["endedAtEpochMs"] = 6200;
  apply_session_activity(restored, report);
  check(restored["games"][0]["playCount"] == 5 && restored["games"][0]["totalPlayTimeMs"] == 13200,
        "A real provider report must update only real totals");
  report = {{"sessionId", "failed-before-running"},
            {"gameId", "g"},
            {"state", "failed"},
            {"fixture", false},
            {"endedAtEpochMs", 8000}};
  apply_session_activity(restored, report);
  check(restored["games"][0]["playCount"] == 5 &&
            restored["history"].back().at("reachedRunning") == false,
        "Failure before Running must not count gameplay");
  const auto summary = game_activity_summary(restored);
  check(summary.at("playCount") == 5 && summary.at("fixturePlayCount") == 1,
        "Home totals must separate fixture and real counts");
}
int main(int argc, char **argv) {
  const auto root = fs::temp_directory_path() / ("xenon-media-activity-" + make_id());
  try {
    check(argc == 2, "Fixture host argument missing");
    pure_activity_tests();
    fs::create_directories(root);
    const auto first = root / "disc1.iso", second = root / "disc2.iso", third = root / "disc3.iso",
               storage = root / "state";
    std::ofstream(first) << "disc one";
    std::ofstream(second) << "disc two";
    std::ofstream(third) << "disc three";
    std::string id, first_id, second_id;
    {
      LauncherService service(storage, false, fs::u8path(argv[1]), {}, nullptr);
      action(service, "settings.set",
             {{"key", "paths/games"}, {"value", path_string(root / "managed")}});
      action(service, "library.import", {{"paths", Json::array({path_string(first)})}});
      auto game = service.snapshot().at("games").front();
      id = game.at("id");
      first_id = game.at("media").front().at("id");
      action(service, "library.media.add",
             {{"id", id}, {"paths", Json::array({path_string(second), path_string(first)})}});
      game = service.snapshot().at("games").front();
      check(game.at("media").size() == 2,
            "Additional disc references must share one game and avoid duplicate sources");
      second_id = game.at("media").back().at("id");
      check(!request(service, "library.media.update",
                     {{"id", id}, {"mediaId", second_id}, {"discNumber", 2}, {"discCount", 1}})
                 .at("ok"),
            "Invalid disc metadata must not commit");
      action(service, "library.media.update",
             {{"id", id},
              {"mediaId", second_id},
              {"discNumber", 2},
              {"discCount", 2},
              {"contentMediaId", "ABCD1234"},
              {"region", "PAL"}});
      action(service, "library.media.select", {{"id", id}, {"mediaId", second_id}});
      check(service.snapshot().at("games").front().at("contentPath") == path_string(second),
            "Primary disc must determine launch source");
      action(service, "session.launch", {{"gameId", id}});
      auto running =
          wait(service, [](const auto &s) { return s.at("session").at("state") == "running"; });
      check(!request(service, "library.media.select", {{"id", id}, {"mediaId", first_id}}).at("ok"),
            "Disc changes must be blocked while the game is running");
      const auto config = read_document(storage / "sessions" /
                                        running.at("session").at("sessionId").get<std::string>() /
                                        "launch-config.json");
      check(config.at("contentPath") == path_string(second) &&
                config.at("primaryMediaId") == second_id && config.at("media").size() == 2,
            "Fixture launch config must include selected disc and all media metadata");
      action(service, "session.stop");
      auto stopped =
          wait(service, [](const auto &s) { return s.at("session").at("state") == "stopped"; });
      check(stopped.at("activitySummary").at("fixturePlayCount") == 1 &&
                stopped.at("activitySummary").at("playCount") == 0,
            "Packaged service must keep test runs out of real playtime");
      action(service, "history.clear");
      check(service.snapshot().at("history").empty(),
            "Service polling must not recreate cleared history");
      action(service, "library.media.select", {{"id", id}, {"mediaId", first_id}});
      auto job =
          action(
              service, "library.transfer",
              {{"id", id}, {"mediaId", second_id}, {"path", path_string(second)}, {"mode", "move"}})
              .at("jobs")
              .back()
              .at("id");
      terminal(service, job);
      game = service.snapshot().at("games").front();
      check(!fs::exists(second) && game.at("contentPath") == path_string(first) &&
                game.at("media").back().at("importMode") == "moved",
            "Moving a secondary disc must keep the primary disc selected");
      check(
          !request(service, "library.media.remove", {{"id", id}, {"mediaId", second_id}}).at("ok"),
          "Managed files must remain reachable until explicitly deleted");
      job = action(service, "library.deleteManaged", {{"id", id}, {"mediaId", second_id}})
                .at("jobs")
                .back()
                .at("id");
      terminal(service, job);
      check(fs::exists(first) &&
                service.snapshot().at("games").front().at("contentPath") == path_string(first),
            "Per-disc managed deletion must preserve other discs and primary selection");
      action(service, "library.media.check", {{"id", id}, {"mediaId", second_id}});
      check(service.snapshot().at("games").front().at("media").back().at("missing") == true,
            "Deleted media must be reported as missing without dropping its reference");
      action(service, "library.media.remove", {{"id", id}, {"mediaId", second_id}});
      job = action(service, "library.media.add",
                   {{"id", id}, {"mode", "move"}, {"paths", Json::array({path_string(third)})}})
                .at("jobs")
                .back()
                .at("id");
      terminal(service, job);
      check(!fs::exists(third) && service.snapshot().at("games").front().at("media").size() == 2,
            "Attach-and-move must preserve grouping");
    }
    {
      LauncherService service(storage, false, fs::u8path(argv[1]), {}, nullptr);
      const auto s = service.snapshot();
      check(s.at("games").front().at("media").front().at("id") == first_id &&
                s.at("history").empty() && s.at("activitySummary").at("fixturePlayCount") == 1,
            "Disc identities, counters and cleared history must survive restart");
    }
    auto old = read_document(storage / "state.json");
    auto &game = old["games"][0];
    game.erase("media");
    game.erase("mediaModelVersion");
    game["managedCopies"] = Json::array();
    game["legacyMetadata"]["media"] = Json::array({Json{{"sourcePath", path_string(first)},
                                                        {"mediaId", "old-a"},
                                                        {"discNumber", 1},
                                                        {"discCount", 2}},
                                                   Json{{"sourcePath", path_string(second)},
                                                        {"mediaId", "old-b"},
                                                        {"discNumber", 2},
                                                        {"discCount", 2}}});
    write_document(storage / "state.json", old);
    {
      LauncherService service(storage, false, {}, {}, nullptr);
      const auto s = service.snapshot();
      check(s.at("games").front().at("media").size() == 2 &&
                read_document(storage / "state.before-media-v1.json") == old,
            "State upgrade must preserve legacy multi-disc records and retain an exact backup");
    }
    fs::remove_all(root);
    std::cout << "Media grouping/metadata/selection/move/delete, runtime linkage, activity "
                 "accounting, history dedupe, legacy upgrade and restart passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << "\nFixtures retained at " << root << '\n';
    return 1;
  }
}
