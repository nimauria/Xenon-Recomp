#include "application/launcher_service.hpp"
#include "core/settings.hpp"
#include <iostream>
using namespace xenon::launcher_next;
namespace fs = std::filesystem;
void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class Operation> void rejects(Operation operation, const char *message) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::exception &) {
    rejected = true;
  }
  check(rejected, message);
}
Json call(LauncherService &service, const std::string &command, Json payload) {
  return service.request(
      {{"version", 1}, {"id", "test"}, {"command", command}, {"payload", payload}});
}
int main() {
  const auto root = fs::temp_directory_path() / ("xenon-profile-settings-" + make_id());
  try {
    const Json global = {{"audio/masterVolume", .8}, {"runtime/graphicsBackend", "Vulkan"}};
    const auto frozen = profile_runtime_values(Json::object(), global, true);
    check(frozen.size() == 9 && frozen.at("audio/masterVolume") == .8,
          "Isolation must capture all nine effective launcher defaults");
    check(effective_profile_settings(
              {{"isolatedSettings", true}, {"runtimeOverrides", {{"audio/masterVolume", .35}}}},
              global)
                  .at("audio/masterVolume") == .35,
          "Isolated profile must override launcher audio");
    check(effective_profile_settings(
              {{"isolatedSettings", false}, {"runtimeOverrides", {{"audio/masterVolume", .35}}}},
              global)
                  .at("audio/masterVolume") == .8,
          "Disabled isolation must use launcher defaults");
    rejects([&] { profile_runtime_values({{"appearance/panelOpacity", .5}}, global); },
            "Unknown profile overrides must be rejected");
    rejects([&] { profile_runtime_values({{"audio/masterVolume", 1.5}}, global); },
            "Profile numeric bounds must match global settings");
    rejects([&] { validate_setting("runtime/graphicsBackend", "Invented renderer"); },
            "Renderer options must be enforced natively");
    rejects([&] { validate_setting("paths/unknownFolder", "path"); },
            "Unknown path settings must not become workflows");
    rejects([&] { validate_setting("community/discordRichPresence", true); },
            "An unavailable provider must not persist a latent Rich Presence opt-in");
    {
      LauncherService service(root);
      check(call(service, "launcher.action",
                 {{"action", "settings.set"},
                  {"data", {{"key", "audio/masterVolume"}, {"value", .8}}}})
                .at("ok"),
            "Settings must work without a mutable schema file in user storage");
      auto created = call(service, "profiles.create",
                          {{"name", std::string(48, 'a')},
                           {"description", std::string(180, 'b')},
                           {"region", "PAL"},
                           {"startupPage", "Support"},
                           {"isolatedSettings", true},
                           {"runtimeOverrides", {{"audio/masterVolume", .35}}}});
      check(created.at("ok"), "Legacy profile limits and overrides should be accepted");
      const auto profile = created.at("data").at("profiles").back();
      const auto id = profile.at("id");
      check(profile.at("effectiveRuntimeSettings").at("audio/masterVolume") == .35,
            "Snapshot must expose the resolved profile setting");
      check(!call(service, "profiles.create", {{"name", std::string(49, 'a')}}).at("ok"),
            "Profile name limit must remain 48 characters");
      check(!call(service, "profiles.create",
                  {{"name", "Too long description"}, {"description", std::string(181, 'b')}})
                 .at("ok"),
            "Profile description limit must remain 180 characters");
      check(call(service, "profiles.activate", {{"id", id}}).at("ok") &&
                service.snapshot()["preferences"]["startPage"] == "Support",
            "Active profile startup choice must override launcher startup page");
      const auto file = root / "portable.xenonprofile";
      check(
          call(service, "launcher.action",
               {{"action", "profiles.export"}, {"data", {{"id", id}, {"path", path_string(file)}}}})
              .at("ok"),
          "Profile export should work");
      check(call(service, "launcher.action",
                 {{"action", "profiles.import"}, {"data", {{"path", path_string(file)}}}})
                .at("ok"),
            "Portable runtime overrides should import");
      check(service.snapshot()["profiles"].back()["runtimeOverrides"]["audio/masterVolume"] == .35,
            "Portable profile must retain runtime overrides");
      const auto act = [&](const std::string &name, const Json &data) {
        return call(service, "launcher.action", {{"action", name}, {"data", data}});
      };
      act("navigation.remember", {{"page", "Captures"}});
      check(service.snapshot()["preferences"]["startPage"] == "Support",
            "Profile startup page applies before remembered-page opt-in");
      act("settings.set", {{"key", "general/restoreLastPage"}, {"value", true}});
      check(service.snapshot()["preferences"]["startPage"] == "Captures",
            "Remembered-page setting takes precedence over profile startup");
      act("notifications.clear", Json::object());
      act("settings.set", {{"key", "audio/masterVolume"}, {"value", .2}});
      act("settings.set", {{"key", "audio/muteUnfocused"}, {"value", true}});
      act("settings.resetKey", {{"key", "audio/masterVolume"}});
      check(!service.snapshot().at("settings").contains("audio/masterVolume") &&
                service.snapshot().at("settings").at("audio/muteUnfocused"),
            "Resetting one setting must preserve neighbouring settings");
      act("notifications.push", {{"title", "Repeat"}, {"message", "Repeated event"}});
      act("notifications.push", {{"title", "Repeat"}, {"message", "Repeated event"}});
      const auto notification = service.snapshot()["notifications"].back();
      check(service.snapshot()["notifications"].size() == 1 && notification["repeatCount"] == 2,
            "Consecutive repeated notifications must collapse within 30 seconds");
      act("notifications.readAll", Json::object());
      check(service.snapshot()["notifications"].back()["read"] == true,
            "Mark all read must persist");
      act("notifications.dismiss", {{"id", notification.at("id")}});
      check(service.snapshot()["notifications"].empty(),
            "Dismiss must remove the requested notification");
    }
    auto dormant = read_document(root / "state.json");
    dormant["settings"]["community/discordRichPresence"] = true;
    write_document(root / "state.json", dormant);
    {
      LauncherService reopened(root, false, {}, {}, nullptr);
      check(reopened.snapshot()["settings"]["community/discordRichPresence"] == false &&
                read_document(root / "state.json")["settings"]["community/discordRichPresence"] ==
                    false,
            "Reopening without a provider must clear an old latent opt-in durably");
    }
    fs::remove_all(root);
    std::cout << "Profile limits, isolated defaults, validation, startup pages and portable "
                 "overrides passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
