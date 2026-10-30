#include "launcher_service.hpp"
#include "core/archive.hpp"
#include "core/launcher_paths.hpp"
#include <algorithm>
#include <chrono>
#include <map>
namespace xenon::launcher_next {
namespace {
std::string now() {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}
bool active(const Json &job) {
  const auto status = job.value("state", "");
  return status == "queued" || status == "running" || status == "cancelling" ||
         status == "committing";
}
std::string operation_title(const std::string &command) {
  static const std::map<std::string, std::string> titles = {
      {"modules.rescan", "Rescan installed modules"},
      {"modules.uninstall", "Uninstall owned module files"},
      {"cache.relocate", "Relocate managed cache"},
      {"filesystem.mount", "Mount filesystem source"},
      {"filesystem.unmount", "Unmount filesystem source"},
      {"filesystem.link", "Register guest link"},
      {"filesystem.unlink", "Remove guest link"},
      {"filesystem.cwd", "Change guest working directory"},
      {"filesystem.test", "Test guest path"},
      {"input.refresh", "Refresh input devices"},
      {"input.reconfigure", "Reconfigure input"},
      {"input.assign", "Assign player device"},
      {"input.automatic", "Choose player device automatically"},
      {"input.clear", "Clear player device"},
      {"input.sourceAdd", "Add player input source"},
      {"input.sourceRemove", "Remove player input source"},
      {"input.profileBind", "Assign player bindings"},
      {"input.profileClear", "Clear player bindings"},
      {"input.vibrate", "Test controller vibration"},
      {"network.refresh", "Refresh network readiness"},
      {"network.fixtureScenario", "Change network test scenario"},
      {"content.identify", "Identify game content"},
      {"content.verify", "Check game content"}};
  const auto found = titles.find(command);
  return found == titles.end() ? "Run host operation" : found->second;
}
} // namespace
void LauncherService::enqueue_service(Json &next, const std::string &command, Json data) {
  const bool local =
      command == "modules.rescan" || command == "modules.uninstall" || command == "cache.relocate";
  if (!local && !host_service_command(command))
    throw std::runtime_error("Unknown runtime-service command");
  const auto group = command.substr(0, command.find('.'));
  const auto snapshot = host_services_->snapshot();
  if (!local && !snapshot.at("capabilities").value(group, false))
    throw std::runtime_error("The corresponding Xenon service adapter is not connected");
  if (command == "modules.uninstall")
    data = prepare_module_uninstall(next, data.at("id").get<std::string>());
  if (command == "cache.relocate")
    data = prepare_cache_relocation(
        next, data.contains("target")
                  ? Json{{"path", data.at("target").value("configuredPath", "")}}
                  : data);
  if (command == "modules.rescan") {
    for (const auto &job : next["jobs"])
      if (active(job) && (job.value("_command", "") == command ||
                          job.value("_command", "") == "modules.uninstall"))
        throw std::runtime_error("Wait for the current module scan or uninstall to finish");
    const auto configured = next["settings"].value("paths/modules", "");
    data = {
        {"configuredRoot", configured},
        {"root", path_string(launcher_destination(storage_, next["settings"], "paths/modules"))},
        {"known", Json::array()}};
    for (const auto &module : next["modules"])
      data["known"].push_back({{"id", module.at("id")}, {"path", module.at("path")}});
  }
  if (command.starts_with("content.")) {
    const auto id = data.at("gameId").get<std::string>();
    const auto game = std::find_if(next["games"].begin(), next["games"].end(),
                                   [&](const auto &row) { return row.at("id") == id; });
    if (game == next["games"].end())
      throw std::runtime_error("The game no longer exists");
    data = {{"gameId", id},
            {"path", game->at("contentPath")},
            {"moduleId", game->value("moduleId", "")}};
  }
  unsigned pending = 0;
  for (const auto &job : next["jobs"])
    if (active(job))
      ++pending;
  if (pending >= 32)
    throw std::runtime_error("The operation queue is full");
  while (next["jobs"].size() >= 100) {
    auto &jobs = next["jobs"];
    auto old = std::find_if(jobs.begin(), jobs.end(), [](const auto &job) {
      return !active(job) && !job.value("_cleanup", false);
    });
    if (old == jobs.end())
      break;
    jobs.erase(old);
  }
  next["jobs"].push_back({{"id", make_id()},
                          {"kind", "service.operation"},
                          {"title", operation_title(command)},
                          {"state", "queued"},
                          {"phase", "Waiting"},
                          {"progress", 0},
                          {"createdAt", now()},
                          {"_command", command},
                          {"_data", data}});
}
void LauncherService::run_service(const Json &job,
                                  const std::shared_ptr<std::atomic_bool> &cancel) {
  const auto id = job.at("id").get<std::string>(), command = job.at("_command").get<std::string>();
  if (command == "modules.rescan") {
    run_module_scan(job, cancel);
    return;
  }
  if (command == "modules.uninstall") {
    run_module_uninstall(job, cancel);
    return;
  }
  if (command == "cache.relocate") {
    run_cache_relocation(job, cancel);
    return;
  }
  update_job(id, {{"phase", "Waiting for host service"}, {"progress", 20}});
  if (cancel->load())
    throw OperationCancelled();
  const auto result = host_services_->execute(command, job.at("_data"), cancel);
  complete_job(id, [&](Json &next) {
    next["serviceResult"] = {{"command", command},
                             {"result", result},
                             {"jobId", id},
                             {"fixture", host_services_->snapshot().at("fixture")}};
    if (command.starts_with("content.")) {
      const auto game_id = job.at("_data").at("gameId");
      for (auto &game : next["games"])
        if (game.at("id") == game_id)
          game["contentReport"] = result;
    }
    if (command == "input.vibrate")
      notify(next, "Input fixture", result.value("message", "Fixture vibration requested"), "info",
             {{"type", "navigate"}, {"page", "Settings"}, {"targetId", "input"}});
  });
}
} // namespace xenon::launcher_next
