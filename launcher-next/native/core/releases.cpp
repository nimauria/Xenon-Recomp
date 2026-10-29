#include "releases.hpp"
#include "platform/http.hpp"
#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
#include <cctype>
namespace xenon::launcher_next {
namespace {
std::vector<std::string> split(const std::string &text, char delimiter) {
  std::vector<std::string> result;
  std::istringstream stream(text);
  std::string item;
  while (std::getline(stream, item, delimiter))
    result.push_back(item);
  return result;
}
bool numeric(const std::string &text) {
  return !text.empty() && text.find_first_not_of("0123456789") == std::string::npos;
}
std::string text(const Json &value, const std::string &key, const std::string &fallback = "") {
  return value.contains(key) && value.at(key).is_string() ? value.at(key).get<std::string>()
                                                          : fallback;
}
bool repository_ok(const std::string &repo) {
  static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*$");
  return repo.size() < 200 && std::regex_match(repo, pattern) &&
         repo.find("..") == std::string::npos;
}
} // namespace
std::optional<Version> Version::parse(std::string text) {
  if (!text.empty() && (text[0] == 'v' || text[0] == 'V'))
    text.erase(0, 1);
  static const std::regex pattern("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(?:-([0-9A-"
                                  "Za-z.-]+))?(?:\\+([0-9A-Za-z.-]+))?$");
  std::smatch match;
  if (!std::regex_match(text, match, pattern))
    return {};
  Version value;
  try {
    for (int i = 1; i < 4; i++)
      value.numbers.push_back(std::stoull(match[i]));
  } catch (...) {
    return {};
  }
  if (match[4].matched) {
    if (match[4].str().front() == '.' || match[4].str().back() == '.')
      return {};
    value.prerelease = split(match[4], '.');
    for (const auto &part : value.prerelease)
      if (part.empty() || (numeric(part) && part.size() > 1 && part[0] == '0'))
        return {};
  }
  if (match[5].matched) {
    if (match[5].str().front() == '.' || match[5].str().back() == '.')
      return {};
    const auto build = split(match[5], '.');
    for (const auto &part : build)
      if (part.empty())
        return {};
  }
  value.normalized = text.substr(0, text.find('+'));
  return value;
}
int Version::compare(const Version &other) const {
  if (numbers != other.numbers)
    return numbers < other.numbers ? -1 : 1;
  if (prerelease.empty() != other.prerelease.empty())
    return prerelease.empty() ? 1 : -1;
  for (std::size_t i = 0; i < std::min(prerelease.size(), other.prerelease.size()); ++i) {
    const auto &a = prerelease[i];
    const auto &b = other.prerelease[i];
    if (a == b)
      continue;
    const bool an = numeric(a), bn = numeric(b);
    if (an != bn)
      return an ? -1 : 1;
    if (an && a.size() != b.size())
      return a.size() < b.size() ? -1 : 1;
    return a < b ? -1 : 1;
  }
  return prerelease.size() == other.prerelease.size()  ? 0
         : prerelease.size() < other.prerelease.size() ? -1
                                                       : 1;
}
std::string host_key() {
#ifdef _WIN32
  std::string result = "windows-";
#elif defined(__APPLE__)
  std::string result = "macos-";
#else
  std::string result = "linux-";
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
  return result + "arm64";
#else
  return result + "x64";
#endif
}
Json catalog_paths(const Json &document) {
  if (document.at("schema") != "xenon.module-catalog" || document.at("version") != 1 ||
      !document.at("modules").is_array() || document.at("modules").size() > 256)
    throw std::runtime_error("Unsupported module catalogue schema");
  const std::regex pattern("^modules/[A-Za-z0-9][A-Za-z0-9._-]*\\.json$");
  std::set<std::string> seen;
  for (const auto &value : document.at("modules")) {
    const auto path = value.get<std::string>();
    if (path.size() > 240 || path.find("..") != std::string::npos ||
        !std::regex_match(path, pattern) || !seen.insert(path).second)
      throw std::runtime_error("Invalid or duplicate catalogue entry path");
  }
  return document.at("modules");
}
Json catalog_entry(const Json &source, const std::string &path, const std::string &host) {
  if (source.at("schema") != "xenon.module-entry" || source.at("version") != 1 ||
      source.at("type") != "game")
    throw std::runtime_error("Unsupported module entry schema");
  const auto id = source.at("id").get<std::string>(), name = source.at("name").get<std::string>(),
             description = source.at("description").get<std::string>();
  if (!std::regex_match(id, std::regex("^[a-z0-9]+(?:[._-][a-z0-9]+)+$")) || name.empty() ||
      name.size() > 400 || description.empty() || description.size() > 2000)
    throw std::runtime_error("Invalid module catalogue identity");
  const auto &repo = source.at("repository");
  if (repo.at("provider") != "github")
    throw std::runtime_error("Unsupported release provider");
  const auto repository =
      repo.at("owner").get<std::string>() + "/" + repo.at("name").get<std::string>();
  if (!repository_ok(repository))
    throw std::runtime_error("Invalid module repository");
  const auto &publisher = source.at("publisher");
  if (!publisher.at("name").is_string() || publisher.at("name").get<std::string>().empty() ||
      !publisher.at("verified").is_boolean())
    throw std::runtime_error("Invalid module publisher");
  const auto &assets = source.at("releaseAssets");
  if (!assets.is_object() || assets.empty())
    throw std::runtime_error("Missing module package declarations");
  for (auto entry = assets.begin(); entry != assets.end(); ++entry)
    if (!std::regex_match(entry.key(), std::regex("^[a-z0-9]+-[a-z0-9_]+$")) ||
        !entry.value().is_string() || entry.value().get<std::string>().size() > 200 ||
        !std::regex_match(entry.value().get<std::string>(),
                          std::regex("^[A-Za-z0-9][A-Za-z0-9._-]*$")))
      throw std::runtime_error("Invalid module package name");
  Json value = {{"id", id},
                {"name", name},
                {"description", description},
                {"repository", repository},
                {"repositoryUrl", "https://github.com/" + repository},
                {"publisher", publisher.at("name")},
                {"verified", publisher.at("verified")},
                {"hostKey", host},
                {"assetName", assets.value(host, "")},
                {"entryPath", path},
                {"launcher", source.value("launcher", Json::object())},
                {"game", source.value("game", Json::object())},
                {"dlc", source.value("dlc", Json::array())},
                {"tags", source.value("tags", Json::array())}};
  if (!value["launcher"].is_object() || !value["game"].is_object() || !value["dlc"].is_array() ||
      value["dlc"].size() > 2048)
    throw std::runtime_error("Invalid module display metadata");
  auto &metadata = value["launcher"];
  for (auto field = metadata.begin(); field != metadata.end();)
    if (!field.key().empty() && field.key()[0] == '_')
      field = metadata.erase(field);
    else
      ++field;
  return value;
}
std::string sha256_digest(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (value.rfind("sha256:", 0) == 0)
    value.erase(0, 7);
  return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos
             ? value
             : "";
}
bool newer_version(const std::string &candidate, const std::string &current) {
  const auto a = Version::parse(candidate), b = Version::parse(current);
  return a && (!b || a->compare(*b) > 0);
}
Json best_release(const Json &releases, const std::string &repository, const std::string &expected,
                  bool include_prerelease) {
  if (!releases.is_array() || releases.size() > 100 || !repository_ok(repository))
    throw std::runtime_error("Invalid GitHub release response");
  Json result = nullptr;
  std::optional<Version> best;
  for (const auto &release : releases) {
    if (release.value("draft", false))
      continue;
    const auto tag = release.value("tag_name", "");
    const auto version = Version::parse(tag);
    if (!version ||
        (!include_prerelease &&
         (release.value("prerelease", false) || !version->prerelease.empty())) ||
        (best && version->compare(*best) <= 0))
      continue;
    result = {{"version", version->normalized},
              {"tag", tag},
              {"name", text(release, "name", tag)},
              {"notes", text(release, "body")},
              {"publishedAt", text(release, "published_at")},
              {"prerelease", release.value("prerelease", false)},
              {"url", "https://github.com/" + repository + "/releases/tag/" + tag},
              {"asset", nullptr}};
    best = version;
    if (!release.contains("assets") || !release["assets"].is_array())
      continue;
    for (const auto &asset : release["assets"])
      if (asset.value("name", "") == expected) {
        const auto url = asset.value("browser_download_url", "");
        const auto prefix = "https://github.com/" + repository + "/releases/download/";
        const auto size = asset.value("size", std::int64_t{});
        if (url.rfind(prefix, 0) != 0 || !allowed_update_url(url) || size <= 0 ||
            size > 512ll * 1024 * 1024)
          continue;
        const auto parts = split(url.substr(prefix.size()), '/');
        auto encoded = tag;
        for (std::size_t position = 0;
             (position = encoded.find('+', position)) != std::string::npos; position += 3)
          encoded.replace(position, 1, "%2B");
        if (parts.size() != 2 || parts[1] != expected || (parts[0] != tag && parts[0] != encoded))
          continue;
        result["asset"] = {{"name", expected},
                           {"url", url},
                           {"size", size},
                           {"sha256", sha256_digest(text(asset, "digest"))}};
        break;
      }
  }
  return result;
}
} // namespace xenon::launcher_next
