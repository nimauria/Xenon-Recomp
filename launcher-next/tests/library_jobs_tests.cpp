#include "application/launcher_service.hpp"
#include "core/archive.hpp"
#include "core/file_transfer.hpp"
#include <fstream>
#include <iostream>
using namespace xenon::launcher_next;
namespace fs = std::filesystem;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
Json request(LauncherService &service, const std::string &command, Json data = Json::object()) {
  return service.request({{"version", 1},
                          {"id", "test"},
                          {"command", "launcher.action"},
                          {"payload", {{"action", command}, {"data", data}}}});
}
Json action(LauncherService &service, const std::string &command, Json data = Json::object()) {
  const auto reply = request(service, command, data);
  if (!reply.at("ok"))
    throw std::runtime_error(reply.at("error").at("message").get<std::string>());
  return reply.at("data");
}
Json terminal(LauncherService &service, const std::string &id) {
  for (unsigned i = 0; i < 2000; ++i) {
    const auto snapshot = service.snapshot();
    for (const auto &job : snapshot.at("jobs"))
      if (job.at("id") == id) {
        const auto status = job.value("state", "");
        if ((status == "completed" || status == "failed" || status == "cancelled") &&
            !job.value("cleaningUp", false))
          return job;
      }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  throw std::runtime_error("Library operation timed out");
}
void put(const fs::path &path, const std::string &value) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << value;
}
int main() {
  const auto root = fs::temp_directory_path() / ("xenon-library-" + make_id());
  try {
    fs::create_directories(root);
    const auto managed = root / "managed";
    const auto storage = root / "state";
    std::string interrupted_job, interrupted_game;
    {
      LauncherService *service_ptr = nullptr;
      std::string mode;
      LauncherService service(
          storage, false, {},
          [&](const std::string &id, const std::string &phase) {
            if (phase == "Content ready" && mode == "change") {
              put(root / "changing/default.xex", "CHANGED after copy");
              put(root / "changing/new-file.txt", "new source content");
              const auto state = service_ptr->snapshot();
              action(*service_ptr, "library.favorite",
                     {{"id", state.at("games").back().at("id")}, {"favorite", true}});
            }
            if (phase == "Content ready" && mode == "cancel-before")
              action(*service_ptr, "jobs.cancel", {{"id", id}});
            if (phase == "Content saved" && mode == "cancel-after")
              action(*service_ptr, "jobs.cancel", {{"id", id}});
            if (phase == "Content ready" && mode == "switch-module")
              action(
                  *service_ptr, "library.update",
                  {{"id", service_ptr->snapshot().at("games").front().at("id")}, {"moduleId", ""}});
          },
          nullptr);
      service_ptr = &service;
      action(service, "settings.set", {{"key", "paths/games"}, {"value", path_string(managed)}});
      put(root / "game/default.xex", "game fixture");
      fs::create_directories(root / "game/empty");
      const auto first =
          action(service, "library.import",
                 {{"paths", Json::array({path_string(root / "game")})}, {"mode", "move"}})
              .at("jobs")
              .back()
              .at("id")
              .get<std::string>();
      check(terminal(service, first).at("state") == "completed", "Move should complete");
      auto state = service.snapshot();
      const auto game = state.at("games").back();
      const auto game_id = game.at("id").get<std::string>();
      check(!fs::exists(root / "game") &&
                fs::is_regular_file(fs::u8path(game.at("contentPath").get<std::string>()) /
                                    "default.xex"),
            "Move must commit and preserve the verified copy");
      check(fs::is_directory(fs::u8path(game.at("contentPath").get<std::string>()) / "empty"),
            "Empty directories must survive");
      check(!request(service, "library.remove", {{"id", game_id}}).at("ok"),
            "Managed entry must remain reachable until files are deleted");
      put(root / "dlc-one", "DLC1");
      put(root / "dlc-two", "DLC2");
      const auto queued = action(
          service, "dlc.import",
          {{"id", game_id},
           {"paths", Json::array({path_string(root / "dlc-one"), path_string(root / "dlc-two")})}});
      for (const auto &job : queued.at("jobs"))
        if (job.value("kind", "") == "library.transfer")
          check(terminal(service, job.at("id")).at("state") == "completed",
                "DLC batch must complete");
      const auto with_dlc = service.snapshot().at("games").back();
      check(with_dlc.at("dlc").size() == 2 && fs::exists(root / "dlc-one") &&
                fs::exists(root / "dlc-two"),
            "DLC must copy without deleting originals");
      const auto first_dlc = with_dlc.at("dlc").front();
      auto verify = action(service, "dlc.verify", {{"id", game_id}, {"dlcId", first_dlc.at("id")}})
                        .at("jobs")
                        .back()
                        .at("id");
      check(terminal(service, verify).at("state") == "completed" &&
                service.snapshot().at("games").back().at("dlc").front().at("receiptVerified"),
            "Unchanged DLC must match its import receipt");
      put(fs::u8path(first_dlc.at("path").get<std::string>()), "modified managed DLC");
      verify = action(service, "dlc.verify", {{"id", game_id}, {"dlcId", first_dlc.at("id")}})
                   .at("jobs")
                   .back()
                   .at("id");
      check(terminal(service, verify).at("state") == "failed" && !service.snapshot()
                                                                      .at("games")
                                                                      .back()
                                                                      .at("dlc")
                                                                      .front()
                                                                      .at("receiptVerified")
                                                                      .get<bool>(),
            "Changed managed content must fail verification and clear its previous receipt result");
      const auto delete_dlc =
          action(service, "dlc.remove", {{"id", game_id}, {"dlcId", first_dlc.at("id")}})
              .at("jobs")
              .back()
              .at("id");
      check(terminal(service, delete_dlc).at("state") == "completed",
            "Managed DLC deletion should complete");
      check(fs::exists(root / "dlc-one") &&
                !fs::exists(fs::u8path(first_dlc.at("path").get<std::string>())),
            "DLC deletion must preserve original source");
      put(managed / "foreign/keep.txt", "unowned sibling");
      const auto delete_all =
          action(service, "library.deleteManaged", {{"id", game_id}}).at("jobs").back().at("id");
      check(terminal(service, delete_all).at("state") == "completed",
            "Managed game deletion should complete");
      check(fs::exists(managed / "foreign/keep.txt") && fs::exists(root / "dlc-two") &&
                service.snapshot().at("games").size() == 1,
            "Deletion must keep foreign files, originals and the library entry");
      // Existing reference retains a concurrent favorite edit, even when source changes before
      // cleanup.
      put(root / "changing/default.xex", "old source");
      action(service, "library.import", {{"paths", Json::array({path_string(root / "changing")})}});
      const auto changing_id = service.snapshot().at("games").back().at("id");
      mode = "change";
      const auto change_job =
          action(service, "library.import",
                 {{"paths", Json::array({path_string(root / "changing")})}, {"mode", "move"}})
              .at("jobs")
              .back()
              .at("id");
      const auto changed = terminal(service, change_job);
      check(changed.at("state") == "completed" && changed.contains("warning"),
            "Changed source should be retained with a warning");
      check(service.snapshot().at("games").back().at("id") == changing_id,
            "Moving an already registered source must reuse the same library entry");
      check(fs::exists(root / "changing/default.xex") &&
                fs::exists(root / "changing/new-file.txt") &&
                service.snapshot().at("games").back().at("favorite"),
            "New and changed files and concurrent state must be preserved");
      // Cancellation before registration never removes originals or registers a partial game.
      put(root / "cancel-before/default.xex", "original");
      mode = "cancel-before";
      const auto before_count = service.snapshot().at("games").size();
      const auto cancel_before =
          action(service, "library.import",
                 {{"paths", Json::array({path_string(root / "cancel-before")})}, {"mode", "move"}})
              .at("jobs")
              .back()
              .at("id");
      check(terminal(service, cancel_before).at("state") == "cancelled" &&
                fs::exists(root / "cancel-before/default.xex") &&
                service.snapshot().at("games").size() == before_count,
            "Cancelled transfer must preserve source and previous state");
      // Cancellation after registration preserves both copies and supports cleanup-only retry after
      // restart.
      put(root / "cancel-after/default.xex", "original");
      mode = "cancel-after";
      interrupted_job =
          action(service, "library.import",
                 {{"paths", Json::array({path_string(root / "cancel-after")})}, {"mode", "move"}})
              .at("jobs")
              .back()
              .at("id");
      check(terminal(service, interrupted_job).at("state") == "cancelled",
            "Committed transfer should cancel safely");
      const auto interrupted = service.snapshot().at("games").back();
      interrupted_game = interrupted.at("id");
      check(fs::exists(root / "cancel-after/default.xex") &&
                fs::exists(fs::u8path(interrupted.at("contentPath").get<std::string>()) /
                           "default.xex"),
            "Committed cancellation must retain both copies");
      check(!request(service, "library.transfer", {{"path", path_string(root)}, {"mode", "move"}})
                 .at("ok"),
            "Transfers must reject sources containing launcher storage");
      mode.clear();
      put(root / "catalog-module/module.json",
          R"({"id":"catalog.module","name":"Catalogue module","version":"1","addOns":[{"dlcId":"mission:one","name":"Mission One","content_ids":["expected-content-id"]}]})");
      action(service, "modules.import", {{"path", path_string(root / "catalog-module")}});
      action(service, "library.update", {{"id", game_id}, {"moduleId", "catalog.module"}});
      check(service.snapshot().at("dlcCatalog").at("catalog.module").front().at("dlcId") ==
                "mission:one",
            "Legacy add-on aliases and stable catalogue IDs must be exposed");
      put(root / "expected/a.bin", "A");
      put(root / "expected/b.bin", "B");
      auto catalog_job = action(service, "dlc.import",
                                {{"id", game_id},
                                 {"catalogId", "mission:one"},
                                 {"paths", Json::array({path_string(root / "expected/a.bin"),
                                                        path_string(root / "expected/b.bin")})}})
                             .at("jobs")
                             .back()
                             .at("id");
      check(terminal(service, catalog_job).at("state") == "completed",
            "Multiple sources for one catalogue add-on must copy together");
      const auto installed = service.snapshot().at("games").front().at("dlc").front();
      const auto old_content = fs::u8path(installed.at("path").get<std::string>());
      check(fs::exists(old_content / "a.bin") && fs::exists(old_content / "b.bin") &&
                installed.at("catalogId") == "mission:one" && !installed.at("verified").get<bool>(),
            "Catalogue selection must keep both sources without claiming runtime validity");
      put(root / "replacement/new.bin", "replacement");
      catalog_job = action(service, "dlc.import",
                           {{"id", game_id},
                            {"catalogId", "mission:one"},
                            {"paths", Json::array({path_string(root / "replacement")})}})
                        .at("jobs")
                        .back()
                        .at("id");
      check(terminal(service, catalog_job).at("state") == "completed",
            "Catalogue add-on replacement must complete");
      const auto replaced = service.snapshot().at("games").front().at("dlc").front();
      check(replaced.at("id") == installed.at("id") && fs::exists(old_content / "a.bin") &&
                fs::exists(fs::u8path(replaced.at("path").get<std::string>()) / "new.bin"),
            "Replacement must retain its stable ID and previous managed copy");
      mode = "switch-module";
      catalog_job = action(service, "dlc.import",
                           {{"id", game_id},
                            {"catalogId", "mission:one"},
                            {"paths", Json::array({path_string(root / "expected/a.bin")})}})
                        .at("jobs")
                        .back()
                        .at("id");
      check(terminal(service, catalog_job).at("state") == "failed" &&
                service.snapshot().at("games").front().at("dlc").front().at("path") ==
                    replaced.at("path"),
            "Changing the selected module during import must keep the previous add-on");
      mode.clear();
    }
    {
      LauncherService service(storage, false, {}, {}, nullptr);
      fs::create_directories(root / "empty-addon");
      const auto empty_job = action(service, "dlc.import",
                                    {{"id", service.snapshot().at("games").front().at("id")},
                                     {"paths", Json::array({path_string(root / "empty-addon")})}})
                                 .at("jobs")
                                 .back()
                                 .at("id");
      check(terminal(service, empty_job).at("state") == "failed",
            "An empty add-on source must not register as imported content");
      const auto retry =
          action(service, "jobs.retry", {{"id", interrupted_job}}).at("jobs").back().at("id");
      check(terminal(service, retry).at("state") == "completed" &&
                !fs::exists(root / "cancel-after"),
            "Committed retry must only finish source cleanup");
      const auto state = service.snapshot();
      const auto game = state.at("games").back();
      check(game.at("id") == interrupted_game && game.at("managedCopies").size() == 1,
            "Retry must not create a second game or managed copy");
      // Forged ownership markers must never permit deletion.
      const auto copy_root =
          fs::u8path(game.at("managedCopies").front().at("root").get<std::string>());
      put(copy_root / "owner.json", "{}");
      const auto removal = action(service, "library.deleteManaged", {{"id", interrupted_game}})
                               .at("jobs")
                               .back()
                               .at("id");
      check(terminal(service, removal).at("state") == "failed" &&
                fs::exists(copy_root / "content/default.xex"),
            "Ownership mismatch must leave files untouched");
    }
    // Core rejection and source-retention checks do not require a runtime provider.
    put(root / "linked/normal.txt", "normal");
    std::error_code error;
    fs::create_directory_symlink(root / "changing", root / "linked/redirect", error);
    if (!error) {
      bool rejected = false;
      try {
        copy_library_content(root / "linked", root / "link-output", [] {}, [](auto, auto) {});
      } catch (...) {
        rejected = true;
      }
      check(rejected, "Transfer must reject nested links");
    }
    std::cout << "launcher_next_library_jobs_tests: ok\n";
    fs::remove_all(root);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << "\nFixtures retained at " << root << '\n';
    return 1;
  }
}
