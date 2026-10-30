#include "support_bundle.hpp"
#include "archive.hpp"
#include "file_transfer.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
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
void replace_private(std::string &value, const std::string &needle) {
  if (needle.empty())
    return;
  auto folded = lower(value);
  const auto key = lower(needle);
  std::size_t offset = 0;
  while ((offset = folded.find(key, offset)) != std::string::npos) {
    value.replace(offset, needle.size(), "<private>");
    folded.replace(offset, needle.size(), "<private>");
    offset += 9;
  }
}
std::string tail(const fs::path &root, const fs::path &file, std::size_t limit) {
  try {
    if (!fs::is_regular_file(file) || redirected_path(file) || fs::weakly_canonical(file) != file)
      return "";
    const auto relative = file.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..")
      return "";
    std::ifstream input(file, std::ios::binary);
    if (!input)
      return "<log could not be read>\n";
    const auto size = fs::file_size(file);
    if (size > limit) {
      input.seekg(static_cast<std::streamoff>(size - limit));
      std::string partial;
      std::getline(input, partial);
    }
    std::string result, line;
    while (std::getline(input, line)) {
      if (line.size() > 8192)
        result += "<oversized log line omitted>\n";
      else
        result += line + '\n';
      if (result.size() > limit + 8192)
        break;
    }
    return result;
  } catch (const std::exception &) {
    return "<log unavailable>\n";
  }
}
bool email_character(unsigned char c) {
  return std::isalnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-';
}
} // namespace
std::string redact_support_text(std::string value, const std::vector<std::string> &private_values) {
  auto needles = private_values;
  std::sort(needles.begin(), needles.end(),
            [](const auto &a, const auto &b) { return a.size() > b.size(); });
  needles.erase(std::unique(needles.begin(), needles.end()), needles.end());
  for (const auto &needle : needles) {
    replace_private(value, needle);
    auto slash = needle;
    std::replace(slash.begin(), slash.end(), '\\', '/');
    replace_private(value, slash);
    std::replace(slash.begin(), slash.end(), '/', '\\');
    replace_private(value, slash);
    std::string escaped;
    for (const auto c : slash) {
      escaped += c;
      if (c == '\\')
        escaped += '\\';
    }
    replace_private(value, escaped);
  }
  // Fixed patterns on bounded lines, never patterns derived from user input.
  static const std::regex windows_home(R"(([A-Za-z]:[\\/]+Users[\\/]+)[^\\/\s"']+)",
                                       std::regex::icase);
  static const std::regex unix_home(R"((/(?:home|Users)/)[^/\s"']+)", std::regex::icase);
  std::istringstream lines(value);
  std::string result, line;
  while (std::getline(lines, line)) {
    const auto folded = lower(line);
    if (folded.find("authorization") != std::string::npos ||
        folded.find("password") != std::string::npos || folded.find("token") != std::string::npos ||
        folded.find("cookie") != std::string::npos || folded.find("secret") != std::string::npos) {
      result += "<credential-bearing line removed>\n";
      continue;
    }
    line = std::regex_replace(line, windows_home, "$1<user>");
    line = std::regex_replace(line, unix_home, "$1<user>");
    for (std::size_t at = 0; (at = line.find('@', at)) != std::string::npos;) {
      auto begin = at, end = at + 1;
      while (begin && email_character(static_cast<unsigned char>(line[begin - 1])))
        --begin;
      while (end < line.size() && (std::isalnum(static_cast<unsigned char>(line[end])) ||
                                   line[end] == '.' || line[end] == '-'))
        ++end;
      if (begin < at && end > at + 1 &&
          line.substr(at + 1, end - at - 1).find('.') != std::string::npos) {
        line.replace(begin, end - begin, "<email>");
        at = begin + 7;
      } else
        ++at;
    }
    result += line + '\n';
  }
  return result;
}
fs::path create_support_bundle(const fs::path &storage, const Json &state) {
  std::vector<std::string> private_values{text(storage)};
  for (const auto *key : {"USERPROFILE", "HOME"})
    if (const auto *value = std::getenv(key))
      private_values.emplace_back(value);
  for (auto value = state.at("settings").begin(); value != state.at("settings").end(); ++value)
    if (value.key().starts_with("paths/") && value.value().is_string())
      private_values.push_back(value.value().get<std::string>());
  for (const auto &profile : state.at("profiles")) {
    const auto name = profile.value("name", "");
    if (name.size() >= 3)
      private_values.push_back(name);
    for (const auto *key : {"gamePath", "savePath", "screenshotPath"})
      if (!profile.value(key, "").empty())
        private_values.push_back(profile.at(key));
  }
  for (const auto &game : state.at("games")) {
    private_values.push_back(game.value("contentPath", ""));
    for (const auto &medium : game.value("media", Json::array())) {
      private_values.push_back(medium.value("sourcePath", ""));
      const auto name = medium.value("name", "");
      if (name.size() >= 3)
        private_values.push_back(name);
    }
    for (const auto &copy : game.value("managedCopies", Json::array()))
      private_values.push_back(copy.value("originalPath", ""));
    for (const auto &dlc : game.value("dlc", Json::array()))
      private_values.push_back(dlc.value("path", ""));
  }
  for (const auto &job : state.at("jobs")) {
    private_values.push_back(job.value("_source", ""));
    for (const auto &source : job.value("_sources", Json::array()))
      if (source.is_string())
        private_values.push_back(source.get<std::string>());
    const auto cache_paths = [&](const Json &location) {
      if (location.is_object())
        for (const auto *key : {"root", "configuredPath"})
          if (location.contains(key) && location.at(key).is_string())
            private_values.push_back(location.at(key));
    };
    cache_paths(job.value("_cacheLocation", Json::object()));
    if (job.value("_command", "") == "cache.relocate" && job.contains("_data")) {
      cache_paths(job.at("_data").value("source", Json::object()));
      cache_paths(job.at("_data").value("target", Json::object()));
    }
  }
  for (const auto &location : state.value("cacheHistory", Json::array()))
    for (const auto *key : {"root", "configuredPath"})
      private_values.push_back(location.value(key, ""));
  if (state.contains("cache"))
    for (const auto *key : {"root", "configuredPath"})
      private_values.push_back(state.at("cache").value(key, ""));
  for (const auto &module : state.at("modules"))
    private_values.push_back(module.value("path", ""));
  Json report = {{"launcher", "Xenon Launcher Next"},
                 {"protocolVersion", 1},
                 {"revision", state.at("revision")},
                 {"profileCount", state.at("profiles").size()},
                 {"gameCount", state.at("games").size()},
                 {"moduleCount", state.at("modules").size()},
                 {"settings", Json::object()},
                 {"modules", Json::array()},
                 {"operations", Json::array()},
                 {"recentSessions", Json::array()}};
#ifdef _WIN32
  report["platform"] = "windows";
#elif defined(__APPLE__)
  report["platform"] = "macos";
#else
  report["platform"] = "linux";
#endif
  for (auto value = state.at("settings").begin(); value != state.at("settings").end(); ++value)
    if (!value.key().starts_with("paths/") && value.key() != "network/baseUrl" &&
        value.key() != "library/lastSelectedGameId")
      report["settings"][value.key()] = value.value();
  for (const auto &module : state.at("modules"))
    report["modules"].push_back({{"id", module.at("id")},
                                 {"version", module.at("version")},
                                 {"enabled", module.value("enabled", true)}});
  for (const auto &job : state.at("jobs"))
    report["operations"].push_back({{"kind", job.value("kind", "")},
                                    {"state", job.at("state")},
                                    {"progress", job.at("progress")},
                                    {"error", job.value("error", "")}});
  for (const auto &session : state.at("history"))
    report["recentSessions"].push_back({{"state", session.value("state", "")},
                                        {"time", session.value("time", "")},
                                        {"fixture", session.value("fixture", false)}});
  if (state.contains("session"))
    report["currentSession"] = {{"state", state.at("session").value("state", "")},
                                {"fixture", state.at("session").value("fixture", false)},
                                {"error", state.at("session").value("lastError", "")}};
  std::map<std::string, std::string> files;
  const auto sanitize = [&](auto &&self, Json &value) -> void {
    if (value.is_string()) {
      auto filtered = redact_support_text(value.get<std::string>(), private_values);
      if (!filtered.empty() && filtered.back() == '\n')
        filtered.pop_back();
      value = filtered;
    } else if (value.is_structured())
      for (auto &child : value)
        self(self, child);
  };
  sanitize(sanitize, report);
  files["diagnostics.json"] = report.dump(2);
  files["privacy.txt"] =
      "Generated locally by Xenon Launcher Next. No files were uploaded. Profile names, game/save "
      "files, configured paths and raw state/launch configurations are not included. Known private "
      "values, home usernames, email-shaped strings and credential-bearing lines are filtered from "
      "bounded log excerpts. Review the files before sharing third-party module output.\n";
  for (const auto *name : {"startup.log", "renderer.log", "cef.log", "update-install.log"}) {
    const auto log = tail(storage, storage / name, 192 * 1024);
    if (!log.empty())
      files[std::string("logs/") + name] = redact_support_text(log, private_values);
  }
  if (state.contains("session")) {
    const auto session_id = state.at("session").value("sessionId", "");
    if (!session_id.empty() && session_id.size() < 100 &&
        session_id.find_first_not_of("0123456789abcdef-") == std::string::npos) {
      const auto log = tail(storage, storage / "sessions" / session_id / "log.txt", 192 * 1024);
      if (!log.empty())
        files["logs/runtime-host.log"] = redact_support_text(log, private_values);
    }
  }
  const auto folder = storage / "support" / make_id();
  fs::create_directories(folder.parent_path());
  if (fs::weakly_canonical(folder.parent_path()) != folder.parent_path() ||
      redirected_path(folder.parent_path()))
    throw std::runtime_error("The support export folder was redirected");
  if (!fs::create_directory(folder))
    throw std::runtime_error("Could not create a new support export folder");
  std::ofstream(folder / "diagnostics.json", std::ios::binary) << files.at("diagnostics.json");
  write_zip(folder / "xenon-support.zip", files);
  return folder / "xenon-support.zip";
}
} // namespace xenon::launcher_next
