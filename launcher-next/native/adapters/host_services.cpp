#include "host_services.hpp"
#include "core/archive.hpp"
#include <algorithm>
#include <fstream>
#include <mutex>
#include <set>
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
std::string text(const fs::path &path) {
  const auto value = path.u8string();
  return {value.begin(), value.end()};
}
std::string lower(std::string value) {
  for (auto &c : value)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return value;
}
std::string string(const Json &data, const char *key, std::size_t limit = 512) {
  const auto value = data.at(key).get<std::string>();
  if (value.empty() || value.size() > limit || value.find('\0') != std::string::npos)
    throw std::runtime_error(std::string("Invalid ") + key);
  return value;
}
std::string guest(const Json &data, const char *key) {
  auto value = string(data, key);
  std::replace(value.begin(), value.end(), '\\', '/');
  const auto colon = value.find(':');
  if (value.front() != '/' &&
      (colon == std::string::npos || !colon ||
       value.substr(0, colon).find_first_not_of(
           "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
           std::string::npos))
    throw std::runtime_error("Use a guest path such as game:/folder");
  if (value.find("../") != std::string::npos || value.ends_with("/..") ||
      value.find("/./") != std::string::npos)
    throw std::runtime_error("Guest paths must not contain traversal components");
  return value;
}
Json network(bool fixture) {
  return {{"clientCompiled", fixture},
          {"configured", false},
          {"transportAvailable", fixture},
          {"connectionState", "disabled"},
          {"connectionLabel", fixture ? "Fixture offline" : "Runtime service unavailable"},
          {"serviceReachable", false},
          {"protocolCompatible", false},
          {"authenticated", false},
          {"authenticationState", "unavailable"},
          {"realtimeState", "disconnected"},
          {"realtimeConnected", false},
          {"onlineServicesReady", false},
          {"protocolVersion", 1},
          {"baseEndpoint", ""},
          {"lastSuccessfulContact", ""},
          {"requestsAttempted", 0},
          {"requestsFailed", 0},
          {"capabilities", Json::array()},
          {"lastError", ""}};
}
Json baseline(bool fixture) {
  return {
      {"version", 1},
      {"fixture", fixture},
      {"revision", 0},
      {"capabilities",
       {{"filesystem", fixture}, {"input", fixture}, {"network", fixture}, {"content", fixture}}},
      {"filesystem",
       {{"initialized", fixture},
        {"mounts", Json::array()},
        {"symbolicLinks", Json::array()},
        {"workingDirectory", ""}}},
      {"input",
       {{"available", fixture},
        {"status", fixture ? "Explicit input fixture; no physical devices are driven"
                           : "Runtime input provider unavailable"},
        {"devices", Json::array()},
        {"users", Json::array()},
        {"profiles", Json::array()},
        {"moduleApi",
         {{"id", "input"},
          {"version", 1},
          {"header", "xenon/input/module_api.hpp"},
          {"cmakeTarget", "Xenon::InputAPI"},
          {"manifestKey", "runtimeApis.input"},
          {"guestXamBridge", false},
          {"features",
           Json::array({"state", "capabilities", "vibration", "power", "device-metadata",
                        "profiles", "multi-source", "flight-devices"})}}}}},
      {"network", network(fixture)}};
}
class Unavailable final : public IHostServices {
public:
  Json snapshot() const override { return baseline(false); }
  Json execute(const std::string &, const Json &,
               const std::shared_ptr<std::atomic_bool> &) override {
    throw std::runtime_error("The corresponding Xenon service adapter is not connected");
  }
};
class Fixture final : public IHostServices {
public:
  explicit Fixture(fs::path root)
      : file_(std::move(root) / "fixture-services.json"), state_(baseline(true)) {
    if (fs::exists(file_)) {
      state_ = read_document(file_, 1024 * 1024);
      if (state_.at("version") != 1 || !state_.value("fixture", false))
        throw std::runtime_error("Unsupported host-services fixture state");
    } else {
      state_["input"]["devices"] = Json::array({{{"id", "fixture-pad"},
                                                 {"name", "Fixture gamepad"},
                                                 {"kind", "gamepad"},
                                                 {"connected", true},
                                                 {"provider", "fixture"}},
                                                {{"id", "fixture-keyboard"},
                                                 {"name", "Fixture keyboard"},
                                                 {"kind", "keyboard"},
                                                 {"connected", true},
                                                 {"provider", "fixture"}}});
      state_["input"]["profiles"] = Json::array({{{"id", "fixture-standard"},
                                                  {"name", "Fixture standard bindings"},
                                                  {"schemaVersion", 1}}});
      for (unsigned i = 0; i < 4; ++i)
        state_["input"]["users"].push_back(
            {{"index", i}, {"automatic", true}, {"sources", Json::array()}, {"profileId", ""}});
      write_document(file_, state_);
    }
  }
  Json snapshot() const override {
    std::lock_guard lock(mutex_);
    auto result = state_;
    result["input"]["profileStorePath"] = text(file_);
    return result;
  }
  Json execute(const std::string &command, const Json &data,
               const std::shared_ptr<std::atomic_bool> &cancel) override {
    std::lock_guard lock(mutex_);
    if (cancel->load())
      throw OperationCancelled();
    if (!host_service_command(command))
      throw std::runtime_error("Unknown runtime-service command");
    auto next = state_;
    Json result = Json::object();
    auto &vfs = next["filesystem"];
    if (command == "filesystem.mount") {
      const auto point = guest(data, "mountPoint"), kind = string(data, "kind", 16);
      if (point.back() != ':' || point.find('/') != std::string::npos)
        throw std::runtime_error("Use a guest mount point such as game:");
      if (kind != "host" && kind != "gdfx" && kind != "stfs")
        throw std::runtime_error("Unknown mount kind");
      const auto source =
          fs::weakly_canonical(fs::absolute(fs::u8path(string(data, "source", 32768))));
      if (kind == "host" ? !fs::is_directory(source) : !fs::is_regular_file(source))
        throw std::runtime_error("Mount source is missing or has the wrong type");
      auto &mounts = vfs["mounts"];
      if (mounts.size() >= 64)
        throw std::runtime_error("The fixture supports up to 64 mounts");
      if (std::any_of(mounts.begin(), mounts.end(), [&](const auto &mount) {
            return lower(mount.at("mountPoint")) == lower(point);
          }))
        throw std::runtime_error("This guest mount point is already in use");
      mounts.push_back({{"mountPoint", point},
                        {"kind", kind},
                        {"source", text(source)},
                        {"readOnly", data.value("readOnly", true)},
                        {"fixture", true}});
    } else if (command == "filesystem.unmount") {
      const auto point = lower(guest(data, "mountPoint"));
      auto &mounts = vfs["mounts"];
      const auto count = mounts.size();
      mounts.erase(
          std::remove_if(mounts.begin(), mounts.end(),
                         [&](const auto &mount) { return lower(mount.at("mountPoint")) == point; }),
          mounts.end());
      if (count == mounts.size())
        throw std::runtime_error("Guest mount point no longer exists");
    } else if (command == "filesystem.link") {
      const auto alias = guest(data, "alias"), target = guest(data, "target");
      auto &links = vfs["symbolicLinks"];
      if (links.size() >= 64)
        throw std::runtime_error("The fixture supports up to 64 links");
      if (lower(alias) == lower(target) ||
          std::any_of(links.begin(), links.end(),
                      [&](const auto &link) { return lower(link.at("alias")) == lower(alias); }))
        throw std::runtime_error("This alias already exists or points to itself");
      links.push_back({{"alias", alias}, {"target", target}});
    } else if (command == "filesystem.unlink") {
      const auto alias = lower(guest(data, "alias"));
      auto &links = vfs["symbolicLinks"];
      const auto count = links.size();
      links.erase(
          std::remove_if(links.begin(), links.end(),
                         [&](const auto &link) { return lower(link.at("alias")) == alias; }),
          links.end());
      if (count == links.size())
        throw std::runtime_error("Guest alias no longer exists");
    } else if (command == "filesystem.cwd")
      vfs["workingDirectory"] = guest(data, "path");
    else if (command == "filesystem.test") {
      auto path = guest(data, "path");
      std::set<std::string> visited;
      for (unsigned depth = 0; depth < 65; ++depth) {
        if (!visited.insert(lower(path)).second)
          throw std::runtime_error("Guest symbolic-link cycle detected");
        bool replaced = false;
        for (const auto &link : vfs["symbolicLinks"]) {
          const auto alias = link.at("alias").get<std::string>();
          if (lower(path) == lower(alias) || lower(path).starts_with(lower(alias) + "/")) {
            path = link.at("target").get<std::string>() + path.substr(alias.size());
            replaced = true;
            break;
          }
        }
        if (!replaced)
          break;
        if (depth == 64)
          throw std::runtime_error("Too many guest symbolic links");
      }
      result = {{"path", data.at("path")},
                {"resolvedGuestPath", path},
                {"exists", false},
                {"fixture", true}};
      for (const auto &mount : vfs["mounts"]) {
        const auto point = mount.at("mountPoint").get<std::string>();
        if (!lower(path).starts_with(lower(point)))
          continue;
        if (mount.at("kind") != "host") {
          result["message"] = "Fixture image mounts do not decode game containers";
          break;
        }
        auto relative = path.substr(point.size());
        while (relative.starts_with('/'))
          relative.erase(0, 1);
        const auto root = fs::weakly_canonical(fs::u8path(mount.at("source").get<std::string>())),
                   file = fs::weakly_canonical(root / fs::u8path(relative));
        const auto inside = file.lexically_relative(root);
        if (inside.empty() || *inside.begin() == "..")
          throw std::runtime_error("Guest path resolved outside the mount source");
        result["exists"] = fs::exists(file);
        result["kind"] = fs::is_directory(file) ? "directory" : "file";
        result["readOnly"] = mount.at("readOnly");
        break;
      }
    } else if (command.starts_with("input.")) {
      if (command != "input.refresh" && command != "input.reconfigure") {
        if (!data.at("user").is_number_integer())
          throw std::runtime_error("Input user must be an integer");
        const auto user = data.at("user").get<std::int64_t>();
        if (user < 0 || user > 3)
          throw std::runtime_error("Input user must be between 0 and 3");
        auto &route = next["input"]["users"][user];
        if (command == "input.automatic") {
          route["automatic"] = true;
          route["sources"] = Json::array();
        } else if (command == "input.clear") {
          route["automatic"] = false;
          route["sources"] = Json::array();
        } else if (command == "input.profileClear")
          route["profileId"] = "";
        else if (command == "input.profileBind") {
          const auto profile = string(data, "profileId", 128);
          const auto &profiles = next["input"]["profiles"];
          if (std::none_of(profiles.begin(), profiles.end(),
                           [&](const auto &row) { return row.at("id") == profile; }))
            throw std::runtime_error("Input profile no longer exists");
          route["profileId"] = profile;
        } else if (command == "input.vibrate") {
          const auto &sources = route["sources"];
          if (!route.value("automatic", false) &&
              std::find(sources.begin(), sources.end(), "fixture-pad") == sources.end())
            throw std::runtime_error("Assign a gamepad before testing vibration");
          result = {
              {"user", user},
              {"fixture", true},
              {"message", "Fixture vibration request recorded; no physical device was driven"}};
        } else {
          const auto device = string(data, "deviceId", 128);
          const auto &devices = next["input"]["devices"];
          if (std::none_of(devices.begin(), devices.end(), [&](const auto &row) {
                return row.at("id") == device && row.value("connected", false);
              }))
            throw std::runtime_error("Input device is missing or disconnected");
          auto &sources = route["sources"];
          route["automatic"] = false;
          if (command == "input.assign")
            sources = Json::array({device});
          else if (command == "input.sourceAdd") {
            if (std::find(sources.begin(), sources.end(), device) == sources.end())
              sources.push_back(device);
          } else if (command == "input.sourceRemove")
            sources.erase(std::remove(sources.begin(), sources.end(), Json(device)), sources.end());
        }
      }
    } else if (command == "network.refresh") {
      result = next["network"];
    } else if (command == "network.fixtureScenario") {
      const auto scenario = string(data, "scenario", 32);
      if (scenario != "offline" && scenario != "transport-only" && scenario != "ready" &&
          scenario != "protocol-mismatch" && scenario != "auth-failed")
        throw std::runtime_error("Unknown network fixture scenario");
      auto status = network(true);
      const bool transport = scenario != "offline",
                 compatible = transport && scenario != "protocol-mismatch",
                 authenticated = scenario == "ready";
      status["configured"] = transport;
      status["serviceReachable"] = transport;
      status["protocolCompatible"] = compatible;
      status["authenticated"] = authenticated;
      status["authenticationState"] = authenticated               ? "authenticated"
                                      : scenario == "auth-failed" ? "failed"
                                                                  : "unauthenticated";
      status["realtimeState"] = authenticated ? "connected" : "disconnected";
      status["realtimeConnected"] = authenticated;
      status["onlineServicesReady"] = authenticated;
      status["connectionState"] = authenticated ? "ready" : transport ? "degraded" : "disabled";
      status["connectionLabel"] = "Fixture: " + scenario;
      if (authenticated)
        status["capabilities"] = Json::array({"authentication", "profile", "presence", "realtime"});
      status["lastError"] = scenario == "protocol-mismatch" ? "Fixture protocol mismatch"
                            : scenario == "auth-failed"     ? "Fixture authentication failed"
                                                            : "";
      next["network"] = status;
    } else if (command == "content.identify" || command == "content.verify") {
      const auto source = fs::u8path(string(data, "path", 32768));
      if (!fs::exists(source))
        throw std::runtime_error("Content source is missing");
      result = {{"fixture", true},
                {"identified", true},
                {"integrityVerified", false},
                {"contentPath", text(fs::absolute(source))},
                {"message", "Fixture source identification only; game integrity and compatibility "
                            "were not checked"}};
    }
    if (cancel->load())
      throw OperationCancelled();
    next["revision"] = next.at("revision").get<std::int64_t>() + 1;
    write_document(file_, next);
    state_ = std::move(next);
    return result;
  }

private:
  fs::path file_;
  Json state_;
  mutable std::mutex mutex_;
};
} // namespace
bool host_service_command(const std::string &command) {
  static const std::set<std::string> commands{
      "filesystem.mount",   "filesystem.unmount",      "filesystem.link",    "filesystem.unlink",
      "filesystem.cwd",     "filesystem.test",         "input.refresh",      "input.reconfigure",
      "input.assign",       "input.automatic",         "input.clear",        "input.sourceAdd",
      "input.sourceRemove", "input.profileBind",       "input.profileClear", "input.vibrate",
      "network.refresh",    "network.fixtureScenario", "content.identify",   "content.verify"};
  return commands.contains(command);
}
std::unique_ptr<IHostServices> unavailable_host_services() {
  return std::make_unique<Unavailable>();
}
std::unique_ptr<IHostServices> fixture_host_services(const fs::path &storage) {
  return std::make_unique<Fixture>(storage);
}
} // namespace xenon::launcher_next
