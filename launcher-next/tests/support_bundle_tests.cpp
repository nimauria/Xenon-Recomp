#include "application/launcher_service.hpp"
#include "core/archive.hpp"
#include "core/support_bundle.hpp"
#include <fstream>
#include <iostream>
using namespace xenon::launcher_next;
namespace fs = std::filesystem;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::string read(const fs::path &file) {
  std::ifstream input(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
int main() {
  const auto root = fs::temp_directory_path() / ("xenon-support-" + make_id());
  try {
    fs::create_directories(root);
    const auto raw =
        std::string("Renderer diagnostic at C:\\Users\\PrivateUser\\Desktop\\game.iso and "
                    "/home/HiddenName/games\nmail alice@example.test\nAuthorization: Bearer "
                    "keep-this-secret\napi_token=keep-this-token\nPilot SecretPilot loaded "
                    "E:/PrivateGames/Game\nSecondary media F:/RegionalCopies/PAL Disc.iso\n"
                    "Copied from G:/ImportedMedia/Disc.iso\nCache failure H:/PendingCache/partial\n"
                    "Old cache I:/PreviousCache/files\nError code 1234 in renderer.cpp:70\n");
    const auto cleaned = redact_support_text(raw, {"SecretPilot", "E:/PrivateGames"});
    for (const auto *secret :
         {"PrivateUser", "HiddenName", "alice@example.test", "keep-this-secret", "keep-this-token",
          "SecretPilot", "E:/PrivateGames"})
      check(cleaned.find(secret) == std::string::npos,
            "Known private values and credentials must be filtered");
    check(cleaned.find("Error code 1234") != std::string::npos,
          "Diagnostic context must survive filtering");
    std::ofstream(root / "renderer.log", std::ios::binary) << raw;
    {
      LauncherService service(root, false, {}, {}, nullptr);
      auto state = service.snapshot();
      state["profiles"][0]["name"] = "SecretPilot";
      state["settings"]["paths/games"] = "E:/PrivateGames";
      state["games"].push_back(
          {{"contentPath", "E:/PrivateGames/Game"},
           {"media", Json::array({{{"sourcePath", "F:/RegionalCopies/PAL Disc.iso"},
                                   {"name", "PAL Disc"}}})},
           {"managedCopies", Json::array({{{"originalPath", "G:/ImportedMedia/Disc.iso"}}})}});
      state["settings"]["network/baseUrl"] = "https://secret.example/?auth=private";
      state["jobs"].push_back({{"id", "support-test"},
                               {"kind", "fixture"},
                               {"state", "failed"},
                               {"progress", 10},
                               {"error", "Authorization: Bearer secret"}});
      state["jobs"].push_back({{"id", "cache-test"},
                               {"kind", "service.operation"},
                               {"state", "failed"},
                               {"progress", 10},
                               {"_command", "cache.relocate"},
                               {"_data", {{"target", {{"root", "H:/PendingCache"}}}}}});
      state["cacheHistory"] = Json::array({{{"root", "I:/PreviousCache"}}});
      const auto zip = create_support_bundle(root, state), destination = root / "unpacked";
      extract_package(zip, destination);
      const auto report = read_document(destination / "diagnostics.json");
      check(report.at("profileCount") == 1, "Filtered diagnostics must remain valid JSON");
      check(!report.at("settings").contains("paths/games") &&
                !report.at("settings").contains("network/baseUrl"),
            "Private path and endpoint settings must not be exported");
      const auto log = read(destination / "logs/renderer.log");
      check(log.find("SecretPilot") == std::string::npos &&
                log.find("keep-this-secret") == std::string::npos &&
                log.find("F:/RegionalCopies/PAL Disc.iso") == std::string::npos &&
                log.find("G:/ImportedMedia/Disc.iso") == std::string::npos &&
                log.find("H:/PendingCache") == std::string::npos &&
                log.find("I:/PreviousCache") == std::string::npos,
            "Included log excerpts must be sanitized");
      check(read(root / "renderer.log") == raw, "Export must not modify source logs");
      check(fs::exists(destination / "privacy.txt"), "Bundle must explain its local contents");
      const auto session_id = make_id();
      const auto outside = root.parent_path() / ("outside-support-" + make_id());
      fs::create_directories(outside);
      std::ofstream(outside / "log.txt") << "Foreign file must not be exported";
      fs::create_directories(root / "sessions");
      std::error_code link_error;
      fs::create_directory_symlink(outside, root / "sessions" / session_id, link_error);
      if (!link_error) {
        state["session"] = {{"sessionId", session_id}, {"state", "failed"}, {"fixture", true}};
        const auto second = create_support_bundle(root, state);
        extract_package(second, root / "unpacked-second");
        check(!fs::exists(root / "unpacked-second/logs/runtime-host.log"),
              "Export must not follow redirected log parent folders");
        fs::remove(root / "sessions" / session_id);
      }
      fs::remove_all(outside);
    }
    fs::remove_all(root);
    std::cout << "Support redaction, credential/email/path filtering, valid JSON, bounded logs and "
                 "original preservation passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
