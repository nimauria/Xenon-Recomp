#include "launcher_service.hpp"
#include "core/archive.hpp"
#include "core/file_transfer.hpp"
#include "core/module_settings.hpp"
#include "core/dlc_catalog.hpp"
#include <algorithm>
#include <map>
#include <regex>
#include <set>
namespace xenon::launcher_next {
namespace fs = std::filesystem;
void LauncherService::run_module_scan(const Json &job,
                                      const std::shared_ptr<std::atomic_bool> &cancel) {
  const auto check = [&] {
    if (cancel->load())
      throw OperationCancelled();
  };
  const auto &options = job.at("_data");
  const auto id = job.at("id").get<std::string>();
  update_job(id, {{"phase", "Scanning installed module manifests"}, {"progress", 10}});
  std::set<fs::path> candidates;
  const auto root = fs::u8path(options.at("root").get<std::string>());
  for (const auto &known : options.at("known"))
    candidates.insert(fs::u8path(known.at("path").get<std::string>()));
  Json warnings = Json::array(), found = Json::array();
  if (fs::exists(root)) {
    if (redirected_path(root) || fs::weakly_canonical(root) != root)
      throw std::runtime_error("Configured module folder was redirected");
    unsigned count = 0;
    for (const auto &entry : fs::directory_iterator(root)) {
      check();
      if (++count > 2048)
        throw std::runtime_error("Module folder exceeds 2,048 discovery entries");
      if (entry.path().filename() == ".xenon-staging" || redirected_path(entry.path()) ||
          !entry.is_directory())
        continue;
      if (fs::is_regular_file(entry.path() / "owner.json")) {
        try {
          const auto marker = read_document(entry.path() / "owner.json", 4096);
          const auto owner_id = marker.value("jobId", "");
          if (marker.value("owner", "") == "xenon-launcher-next-module-install" &&
              !owner_id.empty() && entry.path().filename() == fs::u8path("xenon-" + owner_id)) {
            unsigned nested = 0;
            for (const auto &file : fs::recursive_directory_iterator(entry.path())) {
              check();
              if (++nested > 20010)
                throw std::runtime_error("Managed module tree is too large");
              if (redirected_path(file.path()))
                throw std::runtime_error("Managed module contains a redirected entry");
              const auto name = file.path().filename();
              if (file.is_regular_file() &&
                  (name == "module.json" || name == "xenon-module.json" || name == "manifest.json"))
                candidates.insert(file.path().parent_path());
            }
          }
        } catch (const OperationCancelled &) {
          throw;
        } catch (const std::exception &error) {
          warnings.push_back(path_string(entry.path().filename()) + ": " + error.what());
        }
      }
      candidates.insert(entry.path());
    }
  }
  if (candidates.size() > 2048)
    throw std::runtime_error("Too many module locations to scan");
  static const std::regex identity("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$");
  std::set<std::string> seen;
  for (const auto &path : candidates) {
    check();
    try {
      if (!fs::is_directory(path)) {
        warnings.push_back("A registered module folder is missing");
        continue;
      }
      if (redirected_path(path) || fs::weakly_canonical(path) != path)
        throw std::runtime_error("Module folder was redirected");
      fs::path file;
      for (const auto *name : {"xenon-module.json", "module.json", "manifest.json"})
        if (fs::is_regular_file(path / name)) {
          file = path / name;
          break;
        }
      if (file.empty())
        continue;
      if (redirected_path(file))
        throw std::runtime_error("Manifest was redirected");
      const auto definition = read_document(file, 1024 * 1024);
      (void)normalize_dlc_definitions(definition);
      const auto module_id = definition.value("id", definition.value("moduleId", ""));
      if (!std::regex_match(module_id, identity) || !seen.insert(module_id).second)
        throw std::runtime_error("Invalid or duplicate module identity");
      found.push_back({{"id", module_id},
                       {"path", path_string(path)},
                       {"manifest", definition},
                       {"schema", module_settings_schema(definition)}});
    } catch (const OperationCancelled &) {
      throw;
    } catch (const std::exception &error) {
      warnings.push_back(path_string(path.filename()) + ": " + error.what());
    }
  }
  if (install_observer_)
    install_observer_(id, "Module scan ready");
  check();
  complete_job(id, [&](Json &next) {
    if (next["settings"].value("paths/modules", "") !=
        options.at("configuredRoot").get<std::string>())
      throw std::runtime_error(
          "The configured module folder changed during scanning; rescan the current location");
    unsigned added = 0, refreshed = 0;
    for (const auto &entry : found) {
      const auto module_id = entry.at("id").get<std::string>();
      const auto existing =
          std::find_if(next["modules"].begin(), next["modules"].end(),
                       [&](const auto &module) { return module.at("id") == module_id; });
      const bool known =
          std::any_of(options.at("known").begin(), options.at("known").end(),
                      [&](const auto &module) { return module.at("id") == module_id; });
      if (existing == next["modules"].end() && known)
        continue; // A concurrent unregister wins.
      if (existing != next["modules"].end() && existing->at("path") != entry.at("path"))
        continue;
      const auto &definition = entry.at("manifest"), &schema = entry.at("schema");
      if (existing == next["modules"].end()) {
        next["modules"].push_back({{"id", module_id},
                                   {"path", entry.at("path")},
                                   {"name", definition.value("name", module_id)},
                                   {"version", definition.value("version", "Unknown")},
                                   {"enabled", true},
                                   {"managed", false},
                                   {"manifest", definition},
                                   {"schema", schema},
                                   {"settings", module_settings_values(schema, Json::object())}});
        ++added;
      } else {
        (*existing)["manifest"] = definition;
        (*existing)["schema"] = schema;
        (*existing)["settings"] =
            module_settings_values(schema, existing->value("settings", Json::object()));
        (*existing)["name"] = definition.value("name", module_id);
        (*existing)["version"] = definition.value("version", "Unknown");
        ++refreshed;
      }
    }
    refresh_module_presentations(next);
    next["serviceResult"] = {
        {"command", "modules.rescan"},
        {"jobId", id},
        {"fixture", false},
        {"result", {{"added", added}, {"refreshed", refreshed}, {"warnings", warnings}}}};
    if (!warnings.empty())
      for (auto &operation : next["jobs"])
        if (operation.at("id") == id)
          operation["warning"] =
              "Some module locations could not be read. Their registered data was preserved.";
    notify(next, "Modules rescanned",
           std::to_string(added) + " discovered; " + std::to_string(refreshed) +
               " refreshed. Unreadable registrations were kept.");
  });
}
} // namespace xenon::launcher_next
