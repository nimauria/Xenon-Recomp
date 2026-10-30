#include "application/launcher_service.hpp"
#include "core/archive.hpp"
#include "core/cache_storage.hpp"
#include "core/file_transfer.hpp"
#include "core/legacy_migration.hpp"
#include "core/profile_names.hpp"
#include "core/releases.hpp"
#include "picosha2/picosha2.h"
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
std::string read(const fs::path &file) {
  std::ifstream stream(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}
Json request(LauncherService &service, const std::string &name, Json data = Json::object()) {
  return service.request({{"version", 1},
                          {"id", "test"},
                          {"command", "launcher.action"},
                          {"payload", {{"action", name}, {"data", data}}}});
}
Json action(LauncherService &service, const std::string &name, Json data = Json::object()) {
  const auto reply = request(service, name, data);
  if (!reply.at("ok"))
    throw std::runtime_error(reply.at("error").at("message").get<std::string>());
  return reply.at("data");
}
Json finished(LauncherService &service, const Json &id) {
  for (unsigned i = 0; i < 1500; ++i) {
    const auto state = service.snapshot();
    for (const auto &job : state.at("jobs"))
      if (job.at("id") == id && job.at("state") != "queued" && job.at("state") != "running" &&
          job.at("state") != "cancelling" && job.at("state") != "committing" &&
          !job.value("cleaningUp", false))
        return job;
    std::this_thread::sleep_for(5ms);
  }
  throw std::runtime_error("Profile/cache fixture timed out");
}
Json run(LauncherService &service, const std::string &name, Json data = Json::object()) {
  const auto queued = action(service, name, data);
  return finished(service, queued.at("jobs").back().at("id"));
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
    if (!armed || phase != "Cache relocation ready")
      return;
    entered = true;
    changed.notify_all();
    if (!changed.wait_for(lock, 5s, [&] { return released; }))
      throw std::runtime_error("Cache test gate timed out");
    armed = false;
  }
  void wait() {
    std::unique_lock lock(mutex);
    check(changed.wait_for(lock, 5s, [&] { return entered; }),
          "Cache copy did not reach its commit gate");
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }
};
int main() {
  const auto root = fs::temp_directory_path() / ("xenon-profile-cache-" + make_id()),
             storage = root / "state", network = root / "network",
             destination = root / "chosen cache";
  try {
    check(!path_within(root / "Cache-other/file", root / "Cache"),
          "Containment must compare path components rather than text prefixes");
    check(path_within(root / "Cache/file", root / "Cache/"),
          "Containment must accept a trailing folder separator");
#ifdef _WIN32
    check(path_within(root / "CACHE/file", root / "cache"),
          "Windows containment must compare component casing consistently");
#else
    check(!path_within(root / "CACHE/file", root / "cache"),
          "Case-sensitive platforms must retain distinct folder names");
#endif
    check(profile_name_key("\xC3\x89LITE") == profile_name_key("\xC3\xA9lite"),
          "Accented profile names must compare consistently");
    check(profile_name_key("\xCE\x9F\xCE\xA3") == "\xCE\xBF\xCF\x83",
          "Greek sigma must follow scalar lowercase rather than locale/context tailoring");
    check(profile_name_key("\xC4\xB0") == "i\xCC\x87" &&
              profile_name_key("\xF0\x90\x90\x80") == "\xF0\x90\x90\xA8",
          "Expanded and supplementary mappings must be preserved");
    check(trim_profile_name("\xC2\x85\xC2\xA0 Pilot \xE3\x80\x80") == "Pilot",
          "Unicode whitespace trimming must match preview");
    check(profile_name_key("Stra\xC3\x9F"
                           "e") != profile_name_key("STRASSE"),
          "Lowercase matching must not introduce full case folding");
    bool invalid = false;
    try {
      profile_name_key(std::string("a\0b", 3));
    } catch (const std::exception &) {
      invalid = true;
    }
    check(invalid, "NUL profile names must fail");
    fs::create_directories(network);
    fs::create_directories(destination);
    std::ofstream(destination / "unrelated.txt") << "keep";
    const std::string id = "test.cache.module",
                      base = "https://raw.githubusercontent.com/nimauria/Xenon-Modules/main/",
                      asset = "cache-" + host_key() + ".zip";
    write_document(network / "catalog.json",
                   {{"schema", "xenon.module-catalog"},
                    {"version", 1},
                    {"modules", Json::array({"modules/" + id + ".json"})}});
    write_document(
        network / "entry.json",
        {{"schema", "xenon.module-entry"},
         {"version", 1},
         {"id", id},
         {"name", "Cache fixture"},
         {"type", "game"},
         {"description", "Cache fixture"},
         {"publisher", {{"name", "Fixture"}, {"verified", false}}},
         {"repository", {{"provider", "github"}, {"owner", "fixture"}, {"name", "cache"}}},
         {"releaseAssets", {{host_key(), asset}}},
         {"launcher", {{"tileArt", "assets/test.cache.module/cover.png"}}}});
    std::ofstream(network / "cover.png") << "cached artwork bytes";
    write_zip(network / "package.zip",
              {{"module/module.json", Json({{"id", id}, {"version", "1.0.0"}}).dump()},
               {"module/payload.bin", "package bytes"}});
    const auto bytes = read(network / "package.zip"),
               digest = picosha2::hash256_hex_string(bytes.begin(), bytes.end());
    const std::string url = "https://github.com/fixture/cache/releases/download/v1.0.0/" + asset;
    write_document(network / "releases.json",
                   Json::array({{{"tag_name", "v1.0.0"},
                                 {"draft", false},
                                 {"prerelease", false},
                                 {"assets", Json::array({{{"name", asset},
                                                          {"browser_download_url", url},
                                                          {"size", bytes.size()},
                                                          {"digest", "sha256:" + digest}}})}}}));
    write_document(network / "responses.json",
                   {{base + "catalog.json", {{"file", "catalog.json"}}},
                    {base + "modules/" + id + ".json", {{"file", "entry.json"}}},
                    {base + "assets/test.cache.module/cover.png", {{"file", "cover.png"}}},
                    {"https://api.github.com/repos/fixture/cache/releases?per_page=30",
                     {{"file", "releases.json"}}},
                    {url, {{"file", "package.zip"}}}});
    Json saved;
    fs::path new_root, orphan, foreign;
    {
      Gate gate;
      LauncherService service(
          storage, false, {}, [&](const auto &a, const auto &b) { gate.observe(a, b); },
          fixture_http(network));
      for (const auto *language : {"en", "fr", "de", "es", "auto", "fr-CA"})
        check(action(service, "settings.set", {{"key", "general/language"}, {"value", language}})
                      .at("settings")
                      .at("general/language") == language,
              "Display language must be saved as a stable locale ID");
      for (const auto &value : {Json(42), Json("../fr"), Json(""), Json("French")})
        check(!request(service, "settings.set", {{"key", "general/language"}, {"value", value}})
                   .at("ok"),
              "Malformed display language values must be rejected");
      action(service, "settings.resetKey", {{"key", "general/language"}});
      check(
          !request(service, "settings.set", {{"key", "network/enabled"}, {"value", true}}).at("ok"),
          "A network status fixture must not claim configuration support");
      auto created = action(service, "profiles.create", {{"name", "\xC3\x89lite"}});
      check(created.at("notifications").back().value("programTitle", false),
            "Program notification titles must be identifiable without translating profile names");
      const auto custom_notice =
          action(service, "notifications.push", {{"title", "Settings"}, {"message", "Library"}});
      check(!custom_notice.at("notifications").back().value("programTitle", true),
            "User-supplied notification titles must remain original text");
      const auto profile_id = created.at("profiles").back().at("id");
      check(!request(service, "profiles.create", {{"name", "\xC3\xA9LITE"}}).at("ok"),
            "Native create must reject Unicode-case duplicates");
      auto copied = action(service, "profiles.duplicate", {{"id", profile_id}});
      check(copied.at("profiles").back().at("name") == "\xC3\x89lite copy",
            "Unicode duplication must retain display case");
      check(!request(service, "profiles.update",
                     {{"id", copied.at("profiles").back().at("id")}, {"name", "\xC3\xA9LITE"}})
                 .at("ok"),
            "Native edits must reject the same Unicode duplicate identity");
      write_document(root / "portable.json", {{"schema", "xenon.launcher.profile"},
                                              {"version", 1},
                                              {"profile", {{"profileName", "\xC3\xA9LITE"}}}});
      auto imported =
          action(service, "profiles.import", {{"path", path_string(root / "portable.json")}});
      check(imported.at("profiles").back().at("name") == "\xC3\xA9LITE (2)",
            "Portable imports must resolve Unicode collisions without renaming existing profiles");
      fs::create_directories(root / "legacy/Profiles");
      write_document(
          root / "legacy/Profiles/profiles.json",
          Json::array({{{"profileId", "unicode-old"}, {"profileName", "\xC3\xA9LITE"}}}));
      const auto legacy = legacy_snapshot(root / "legacy", {});
      const auto plan =
          legacy_plan(legacy, read_document(storage / "state.json"), make_id(), false, false);
      check(plan.dump().find("(3)") != std::string::npos,
            "Legacy migration must resolve the same Unicode profile collisions");
      check(run(service, "catalog.refresh").at("state") == "completed",
            "Fixture catalogue must refresh");
      check(run(service, "modules.checkUpdate", {{"id", id}}).at("state") == "completed",
            "Fixture release must load");
      check(run(service, "modules.downloadUpdate", {{"id", id}}).at("state") == "completed",
            "Fixture package must download");
      const auto before = read_document(storage / "state.json");
      const auto original_package =
          fs::u8path(before.at("moduleUpdates").at(id).at("_packagePath").get<std::string>());
      gate.arm();
      auto queued = action(service, "settings.set",
                           {{"key", "paths/cache"}, {"value", path_string(destination)}});
      const auto first_id = queued.at("jobs").back().at("id");
      gate.wait();
      check(service.snapshot().at("cache").at("effectivePath") == path_string(storage) &&
                service.snapshot().at("cache").at("pending") == true,
            "Copying must retain the active source cache");
      check(!request(service, "catalog.refresh").at("ok"),
            "Network writes must not race relocation");
      action(service, "profiles.create", {{"name", "During cache copy"}});
      action(service, "jobs.cancel", {{"id", first_id}});
      gate.release();
      check(finished(service, first_id).at("state") == "cancelled" && fs::exists(original_package),
            "Cancellation must preserve source downloads");
      saved = read_document(storage / "state.json");
      const auto cancelled_root =
          cache_root(storage, saved.at("jobs").back().at("_data").at("target"));
      check(!fs::exists(cancelled_root),
            "Cancelled output must be cleaned using its ownership marker");
      queued = action(service, "jobs.retry", {{"id", first_id}});
      check(finished(service, queued.at("jobs").back().at("id")).at("state") == "completed",
            "Cache relocation retry must complete");
      saved = read_document(storage / "state.json");
      new_root = cache_root(storage, saved.at("cache"));
      const auto moved_package =
          fs::u8path(saved.at("moduleUpdates").at(id).at("_packagePath").get<std::string>());
      check(new_root.parent_path() == destination && read(moved_package) == bytes &&
                fs::exists(original_package),
            "Copied package bytes and original must be preserved");
      const auto artwork = service.asset_path("catalog-tile", id);
      check(artwork.has_value() && artwork->parent_path() == new_root / "catalog-art",
            "Artwork must serve from the relocated cache");
      check(service.snapshot().at("profiles").back().at("name") == "During cache copy" &&
                read(destination / "unrelated.txt") == "keep",
            "Cache commit must preserve concurrent profiles and unrelated destination files");
      check(run(service, "catalog.refresh").at("state") == "completed" &&
                service.asset_path("catalog-tile", id)->parent_path() == new_root / "catalog-art",
            "Future catalogue writes must use the selected cache");
      check(run(service, "modules.downloadUpdate", {{"id", id}}).at("state") == "completed",
            "Future downloads must write successfully into the selected cache");
      const auto future_state = read_document(storage / "state.json");
      const auto future_package =
          fs::u8path(future_state.at("moduleUpdates").at(id).at("_packagePath").get<std::string>());
      check(future_package.parent_path().parent_path() == new_root / "downloads",
            "Future download package paths must be recorded under the selected cache");
      check(run(service, "modules.installDownloaded", {{"id", id}}).at("state") == "completed",
            "A verified copied package must still install");
      check(run(service, "settings.resetKey", {{"key", "paths/cache"}}).at("state") == "completed",
            "Reset must copy back to a managed default location");
      saved = read_document(storage / "state.json");
      new_root = cache_root(storage, saved.at("cache"));
      check(new_root.parent_path() == storage && fs::exists(original_package),
            "Reset must retain previous caches and update the default location");
      // Seed interrupted private-copy journals, one owned and one with a changed marker.
      auto sample = saved.at("jobs").back();
      sample["state"] = "running";
      sample.erase("endedAt");
      sample["id"] = make_id();
      saved["jobs"].push_back(sample); // An interrupted journal may already name the active cache.
      const auto owner = make_id();
      orphan = destination / ("xenon-cache-" + owner);
      fs::create_directories(orphan);
      write_document(orphan / "owner.json",
                     {{"owner", "xenon-launcher-next-cache"}, {"id", owner}});
      std::ofstream(orphan / "partial") << "partial";
      sample["id"] = make_id();
      sample["_data"]["target"] = {{"version", 1},
                                   {"root", path_string(orphan)},
                                   {"ownerId", owner},
                                   {"configuredPath", path_string(destination)}};
      saved["jobs"].push_back(sample);
      const auto other = make_id();
      foreign = destination / ("xenon-cache-" + other);
      fs::create_directories(foreign);
      write_document(foreign / "owner.json", {{"owner", "foreign"}});
      std::ofstream(foreign / "keep") << "keep";
      sample["id"] = make_id();
      sample["_data"]["target"] = {{"version", 1},
                                   {"root", path_string(foreign)},
                                   {"ownerId", other},
                                   {"configuredPath", path_string(destination)}};
      saved["jobs"].push_back(sample);
    }
    saved["profiles"].push_back({{"id", "older-unicode-duplicate"},
                                 {"name", "\xC3\xA9LITE"},
                                 {"description", "Preserved old profile"}});
    write_document(storage / "state.json", saved);
    {
      LauncherService restored(storage, false, {}, {}, nullptr);
      const auto updated = action(restored, "profiles.update",
                                  {{"id", "older-unicode-duplicate"},
                                   {"name", "\xC3\xA9LITE"},
                                   {"description", "Metadata edit without renaming"}});
      check(updated.at("profiles").back().at("name") == "\xC3\xA9LITE" &&
                updated.at("profiles").back().at("description") == "Metadata edit without renaming",
            "Older Unicode-equivalent display names must remain editable without forced renaming");
      const auto orphan_id = saved.at("jobs")[saved.at("jobs").size() - 2].at("id"),
                 foreign_id = saved.at("jobs").back().at("id");
      check(finished(restored, orphan_id).at("state") == "interrupted" && !fs::exists(orphan),
            "Startup must clean only unregistered owned partial cache output");
      const auto failed = finished(restored, foreign_id);
      check(failed.contains("cleanupError") &&
                !failed.at("cleanupError").get<std::string>().empty() &&
                fs::exists(foreign / "keep"),
            "Foreign interrupted output must be preserved with a visible cleanup error");
      check(cache_root(storage, read_document(storage / "state.json").at("cache")) == new_root &&
                restored.asset_path("catalog-tile", id).has_value(),
            "Restart must retain the active cache and its artwork");
    }
    fs::remove_all(root);
    std::cout << "Unicode identity/create/copy/import/migration, cache copy/cancel/retry/reset, "
                 "package-reference continuity, unrelated preservation and interrupted cleanup "
                 "ownership passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << "\nFixtures retained at " << root << '\n';
    return 1;
  }
}
