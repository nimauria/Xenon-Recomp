#include "application/launcher_service.hpp"
#include "core/legacy_migration.hpp"
#include "platform/legacy_storage.hpp"
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>
using namespace xenon::launcher_next;
namespace fs = std::filesystem;
void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
Json action(LauncherService &service, const std::string &name, Json data = Json::object()) {
  auto reply = service.request({{"version", 1},
                                {"id", "test"},
                                {"command", "launcher.action"},
                                {"payload", {{"action", name}, {"data", data}}}});
  if (!reply.at("ok"))
    throw std::runtime_error(reply.at("error").at("message").get<std::string>());
  return reply.at("data");
}
Json terminal(LauncherService &service, const std::string &id) {
  for (unsigned attempt = 0; attempt < 1000; ++attempt) {
    const auto snapshot = service.snapshot();
    for (const auto &job : snapshot.at("jobs"))
      if (job.at("id") == id) {
        const auto state = job.value("state", "");
        if (state == "completed" || state == "failed" || state == "cancelled" ||
            state == "interrupted")
          return job;
      }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("Legacy migration job timed out");
}
Json inspect(LauncherService &service, const fs::path &root) {
  const auto state = action(service, "migration.inspect", {{"path", path_string(root)}});
  const auto job = terminal(service, state.at("jobs").back().at("id"));
  check(job.at("state") == "completed", "Inspection should finish");
  return service.snapshot().at("migration");
}
Json apply_review(LauncherService &service, const Json &review) {
  const auto state = action(service, "migration.apply", {{"id", review.at("id")}});
  return terminal(service, state.at("jobs").back().at("id"));
}
int main() {
  const auto root = fs::temp_directory_path() / ("xenon-migration-" + make_id());
  const auto source = root / "old", storage = root / "next";
  auto cancel = std::make_shared<std::atomic_bool>(false);
  try {
    fs::create_directories(root / "empty-old");
    bool empty_rejected = false;
    try {
      legacy_snapshot(root / "empty-old", cancel);
    } catch (const std::exception &) {
      empty_rejected = true;
    }
    check(empty_rejected, "An empty folder must not be offered as a legacy settings migration");
    fs::create_directories(source / "Profiles");
    fs::create_directories(source / "Modules/example");
    fs::create_directories(source / "Games/example");
    const auto avatar = source / "Profiles/avatar image.png";
    std::ofstream(avatar) << "fixture avatar bytes";
    auto uri = "file:///" + path_string(avatar);
    std::replace(uri.begin(), uri.end(), '\\', '/');
    const auto space = uri.find(' ');
    uri.replace(space, 1, "%20");
    const Json settings = {{"ui/theme", "carbon"},
                           {"frontend/audio/masterVolume", "0.4"},
                           {"frontend/input/deadzone", "bad number"},
                           {"frontend/accessibility/reduceMotion", "true"},
                           {"modules/enabled/test.legacy", "false"},
                           {"modules/settings/test.legacy/quality", 2}};
    write_document(source / "settings.json", settings);
    auto profiles = Json::array({{{"profileId", "profile-primary"},
                                  {"profileName", "Legacy Pilot"},
                                  {"description", "From the previous launcher"},
                                  {"active", true},
                                  {"avatarPath", uri},
                                  {"avatarFocalX", .25},
                                  {"avatarZoom", 2.},
                                  {"isolatedSettings", true},
                                  {"runtimeOverrides", {{"audio/masterVolume", .3}}},
                                  {"region", "PAL"},
                                  {"startupPage", "Home"}}});
    write_document(source / "Profiles/profiles.json", profiles);
    write_document(
        source / "library.json",
        Json::array(
            {{{"gameId", "old-game"},
              {"title", "Old game"},
              {"contentPath", path_string(source / "Games/example")},
              {"favorite", true},
              {"moduleId", "test.legacy"},
              {"totalPlayTimeMs", 12000},
              {"playCount", 3},
              {"media", Json::array({Json{{"sourcePath", path_string(source / "Games/example")},
                                          {"discNumber", 1},
                                          {"discCount", 2}},
                                     Json{{"sourcePath", path_string(source / "Games/disc2.iso")},
                                          {"discNumber", 2},
                                          {"discCount", 2},
                                          {"mediaId", "legacy-disc-two"}}})}}}));
    write_document(source / "Modules/example/module.json",
                   {{"id", "test.legacy"},
                    {"name", "Legacy module"},
                    {"version", "1.0.0"},
                    {"settings", Json::array({{{"id", "quality"},
                                               {"type", "choice"},
                                               {"default", 1},
                                               {"options", Json::array({1, 2})}}})}});
    const auto original = legacy_snapshot(source, cancel);
    const auto original_avatar = migration_file_hash(avatar, cancel);
    std::ofstream(source / "settings.ini") << "[ui]\ntheme=carbon\n[frontend]\npaths\\games=\"C:"
                                              "\\\\Games\"\n[general]\nstartupPage=Home\n";
    const auto ini = read_legacy_ini(source / "settings.ini");
    check(ini.at("frontend/paths/games") == "C:\\Games",
          "INI group/key and path escapes must survive without Qt");
    {
      LauncherService service(storage, false, {}, {}, nullptr);
      action(service, "settings.set", {{"key", "audio/masterVolume"}, {"value", .9}});
      auto review = inspect(service, source);
      check(review["counts"]["profiles"] == 1 && review["counts"]["games"] == 1 &&
                review["counts"]["avatars"] == 1 && review["counts"]["modules"] == 1,
            "Review must report actual converted records");
      check(!review["warnings"].empty(), "Invalid settings must be reported in review");
      profiles[0]["description"] = "Changed after review";
      write_document(source / "Profiles/profiles.json", profiles);
      check(apply_review(service, review).at("state") == "failed" &&
                service.snapshot()["profiles"].size() == 1,
            "Source changes after review must abort without importing");
      write_document(source / "Profiles/profiles.json", original.at("profiles"));
      review = inspect(service, source);
      action(service, "settings.set", {{"key", "audio/masterVolume"}, {"value", .8}});
      check(apply_review(service, review).at("state") == "failed" &&
                service.snapshot()["settings"]["audio/masterVolume"] == .8,
            "Edits after review must be retained");
      review = inspect(service, source);
      const auto done = apply_review(service, review);
      if (done.at("state") != "completed")
        throw std::runtime_error(done.dump());
      const auto state = service.snapshot(), profile = state.at("profiles").back();
      check(state["settings"]["audio/masterVolume"] == .8 &&
                state["settings"]["ui/theme"] == "carbon",
            "Default import must keep current settings and bring old appearance across");
      check(state["activeProfileId"] == profile.at("id") &&
                profile["effectiveRuntimeSettings"]["audio/masterVolume"] == .3,
            "Active profile and runtime overrides must survive");
      check(state["games"][0]["favorite"] == true &&
                state["games"][0]["legacyMetadata"]["totalPlayTimeMs"] == 12000,
            "Library favourites and untouched legacy metadata must survive");
      check(state["games"][0]["totalPlayTimeMs"] == 12000 && state["games"][0]["playCount"] == 3 &&
                state["games"][0]["media"].size() == 2 &&
                state["games"][0]["media"][1]["mediaId"] == "legacy-disc-two",
            "Imported media and activity must also be exposed by the new launcher model");
      check(state["modules"][0]["enabled"] == false &&
                state["modules"][0]["settings"]["quality"] == 2,
            "Module enablement and typed settings must survive");
      const auto file = service.asset_path("avatars", profile.at("id"));
      check(file && migration_file_hash(*file, cancel) == original_avatar,
            "Private avatar copy must preserve original bytes");
      const auto backup =
          read_document(fs::u8path(state["migration"]["backupPath"].get<std::string>()));
      check(backup["profiles"].size() == 1 && backup["games"].empty(),
            "Backup must precede the imported state");
      review = inspect(service, source);
      check(review["counts"]["profiles"] == 0 && review["counts"]["games"] == 0 &&
                review["counts"]["modules"] == 0,
            "Repeated imports must not duplicate entries");
      check(apply_review(service, review).at("state") == "completed",
            "Idempotent import should finish");
    }
    check(legacy_snapshot(source, cancel).at("fingerprint") == original.at("fingerprint"),
          "The old launcher data must remain unchanged");
    {
      LauncherService service(storage, false, {}, {}, nullptr);
      check(service.snapshot()["profiles"].size() == 2, "Imported state must survive a restart");
    }
    // Cancellation after copying must remove only the transaction's unreferenced copies.
    {
      std::promise<void> reached, release;
      auto gate = release.get_future().share();
      LauncherService service(
          root / "cancel-next", false, {},
          [&](const std::string &, const std::string &phase) {
            if (phase == "migration.before-commit") {
              reached.set_value();
              gate.wait();
            }
          },
          nullptr);
      const auto review = inspect(service, source);
      const auto state = action(service, "migration.apply", {{"id", review.at("id")}});
      const auto id = state["jobs"].back().at("id").get<std::string>();
      auto future = reached.get_future();
      check(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
            "Migration observer should reach commit gate");
      action(service, "settings.set", {{"key", "audio/masterVolume"}, {"value", .6}});
      action(service, "jobs.cancel", {{"id", id}});
      release.set_value();
      check(terminal(service, id).at("state") == "cancelled" &&
                service.snapshot()["profiles"].size() == 1,
            "Cancelled import must not install profiles");
      check(service.snapshot()["settings"]["audio/masterVolume"] == .6,
            "Concurrent changes during copy must survive cancellation");
      check(fs::is_empty(root / "cancel-next/avatars"),
            "Cancelled import must clean its private image copies");
    }
    // Startup cleans a correctly marked interrupted copy, retaining foreign files.
    {
      const auto next = root / "recover-next";
      const auto id = make_id(), plan = make_id(), key = "legacy-" + plan + "-0.png";
      fs::create_directories(next / "avatars");
      std::ofstream(next / "avatars" / key) << "interrupted";
      std::ofstream(next / "avatars/foreign.png") << "preserve";
      write_document(next / "migration/journals" / (id + ".json"),
                     {{"owner", "xenon-launcher-next-migration"},
                      {"jobId", id},
                      {"planId", plan},
                      {"state", "preparing"},
                      {"avatars", Json::array({key})}});
      LauncherService service(next, false, {}, {}, nullptr);
      check(!fs::exists(next / "avatars" / key) && fs::exists(next / "avatars/foreign.png"),
            "Recovery must clean only marked unreferenced copies");
    }
    fs::remove_all(root);
    std::cout << "Legacy review, source/target revalidation, backup, metadata/settings/avatar "
                 "migration, idempotency, cancellation and recovery passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
