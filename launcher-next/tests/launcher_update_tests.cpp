#include "core/launcher_update.hpp"
#include "core/archive.hpp"
#include "core/releases.hpp"
#include "picosha2/picosha2.h"
#include <fstream>
#include <iostream>
#include <sstream>
using namespace xenon::launcher_next;
namespace fs = std::filesystem;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(F operation, const char *message) {
  bool failed = false;
  try {
    operation();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed, message);
}
std::string text(const fs::path &p) {
  const auto u = p.u8string();
  return {u.begin(), u.end()};
}
std::string digest(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  std::stringstream value;
  value << input.rdbuf();
  return picosha2::hash256_hex_string(value.str());
}
std::map<std::string, std::string> package(const std::string &version, const std::string &marker) {
  std::map<std::string, std::string> result;
  for (const auto *name :
       {"xenon_launcher_next.exe", "xenon_launcher_next.dll", "xenon_launcher_updater.exe",
        "libcef.dll", "icudtl.dat", "resources.pak", "v8_context_snapshot.bin",
        "chrome_100_percent.pak", "locales/en-US.pak", "web/index.html", "settings-schema.json",
        "module-catalog-fallback.json"})
    result[name] = marker;
  result["launcher-package.json"] = Json{
      {"product", "xenon-launcher-next"},
      {"stateSchema", 1},
      {"format", 1},
      {"version", version},
      {"platform", host_key()}}.dump();
  return result;
}
void install(const fs::path &root, const std::map<std::string, std::string> &entries) {
  for (const auto &[relative, contents] : entries) {
    const auto file = root / fs::u8path(relative);
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << contents;
  }
}
int main() {
#ifndef _WIN32
  std::cout
      << "Automatic replacement is Windows-only; platform capability checked by the service.\n";
  return 0;
#else
  const auto root = fs::temp_directory_path() / ("xenon-updater-" + make_id());
  try {
    for (const auto &scenario : {"success", "no-ready", "bad-token", "exit", "rename-failure",
                                 "tampered", "parent-running", "bad-package", "bad-digest"}) {
      const auto folder = root / scenario, current = folder / "installation",
                 storage = folder / "user-data", archive = folder / "update.zip";
      install(current, package("0.1.0", "old"));
      fs::create_directories(storage);
      std::ofstream(storage / "keep.txt") << "user data";
      check(update_installation_supported(current), "Complete package should support updates");
      auto entries = package("0.2.0", "new");
      if (std::string(scenario) == "bad-package")
        entries.erase("xenon_launcher_next.dll");
      write_zip(archive, entries);
      if (std::string(scenario) == "bad-package" || std::string(scenario) == "bad-digest") {
        rejects(
            [&] {
              prepare_launcher_update(archive,
                                      std::string(scenario) == "bad-digest" ? std::string(64, '0')
                                                                            : digest(archive),
                                      "0.2.0", current, storage, {11, 22});
            },
            "Invalid package must be rejected before replacing installation");
        check(read_document(current / "launcher-package.json").at("version") == "0.1.0",
              "Rejected package must preserve previous install");
        continue;
      }
      const auto plan_file =
          prepare_launcher_update(archive, digest(archive), "0.2.0", current, storage, {11, 22});
      const auto plan = read_document(plan_file);
      bool alive = true, stopped = false;
      unsigned restored = 0;
      UpdateHooks hooks;
      hooks.parent_wait_ms = 1;
      hooks.ready_wait_ms = 1;
      hooks.running = [&](ProcessIdentity identity) {
        if (identity.pid == 11)
          return std::string(scenario) == "parent-running";
        return alive;
      };
      hooks.stop = [&](ProcessIdentity identity) {
        check(identity.pid == 33 && identity.created == 44,
              "Only replacement process may be stopped");
        stopped = true;
        alive = false;
        return true;
      };
      hooks.launch = [&](const fs::path &executable,
                         const std::vector<std::string> &args) -> ProcessIdentity {
        check(executable == current / "xenon_launcher_next.exe",
              "Must launch exact package entrypoint");
        if (args.size() == 1) {
          ++restored;
          return {55, 66};
        }
        check(args.size() == 3, "Replacement needs data-dir and receipt handshake");
        if (std::string(scenario) == "exit")
          alive = false;
        if (std::string(scenario) != "no-ready" && std::string(scenario) != "exit")
          write_document(
              plan_file.parent_path() / "ready.json",
              {{"token",
                std::string(scenario) == "bad-token" ? "wrong" : plan.at("id").get<std::string>()},
               {"version", "0.2.0"},
               {"phase", "interface-ready"},
               {"pid", 33},
               {"created", 44}});
        return {33, 44};
      };
      if (std::string(scenario) == "rename-failure")
        hooks.phase = [](const std::string &phase) {
          if (phase == "previous-version-saved")
            throw std::runtime_error("Injected replacement failure");
        };
      if (std::string(scenario) == "tampered")
        std::ofstream(plan_file.parent_path() / "payload/web/index.html") << "tampered";
      if (std::string(scenario) == "success") {
        apply_launcher_update(plan_file, hooks);
        check(read_document(current / "launcher-package.json").at("version") == "0.2.0" &&
                  fs::exists(plan_file.parent_path() / "backup/xenon_launcher_next.exe"),
              "Successful update must retain previous installation");
        check(read_document(plan_file.parent_path() / "status.json").at("state") == "completed" &&
                  !stopped,
              "Matching ready receipt must complete without terminating the replacement");
        rejects([&] { apply_launcher_update(plan_file, hooks); },
                "Completed update cannot be applied again");
      } else {
        rejects([&] { apply_launcher_update(plan_file, hooks); },
                "Failed startup or changed package must abort");
        check(read_document(current / "launcher-package.json").at("version") == "0.1.0",
              "Failed update must restore previous installation");
        const bool before_replace =
            std::string(scenario) == "tampered" || std::string(scenario) == "parent-running";
        check(restored == (before_replace ? 0u : 1u),
              "Restored launcher must reopen after a failed replacement");
        if (std::string(scenario) == "no-ready" || std::string(scenario) == "bad-token")
          check(stopped, "Unready live replacement must stop before rollback");
      }
      check(fs::exists(storage / "keep.txt"), "Updater must not replace or delete user data");
    }
    // Workspace and product boundaries cannot be supplied by an unrelated folder.
    const auto foreign = root / "foreign";
    fs::create_directories(foreign);
    std::ofstream(foreign / "keep.txt") << "foreign";
    check(!update_installation_supported(foreign),
          "Unmarked directory must not qualify as a launcher install");
    rejects([&] { acknowledge_launcher_update(foreign / "ready.json", "invented"); },
            "Receipt must belong to a validated plan");
    std::cout << "launcher_next_update_tests: ok\n";
    fs::remove_all(root);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << "\nFixtures retained at " << root << '\n';
    return 1;
  }
#endif
}
