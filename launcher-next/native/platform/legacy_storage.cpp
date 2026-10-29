#include "legacy_storage.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
std::string utf8(const fs::path &path) {
  const auto s = path.u8string();
  return {s.begin(), s.end()};
}
std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  return first == std::string::npos
             ? ""
             : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
std::string unquote(std::string value) {
  value = trim(value);
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    value = value.substr(1, value.size() - 2);
  std::string result;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '\\' || i + 1 == value.size()) {
      result += value[i];
      continue;
    }
    const char c = value[++i];
    if (c == 'n')
      result += '\n';
    else if (c == 'r')
      result += '\r';
    else if (c == 't')
      result += '\t';
    else if (c == 'x') {
      unsigned code = 0, digits = 0;
      while (i + 1 < value.size() && digits < 4 &&
             std::isxdigit(static_cast<unsigned char>(value[i + 1]))) {
        const auto digit = value[++i];
        code = code * 16 + (digit <= '9' ? digit - '0' : (digit | 32) - 'a' + 10);
        ++digits;
      }
      if (!digits || code == 0 || (code >= 0xD800 && code <= 0xDFFF))
        throw std::runtime_error("Unsupported legacy INI escape");
      if (code < 0x80)
        result += static_cast<char>(code);
      else if (code < 0x800) {
        result += static_cast<char>(0xC0 | (code >> 6));
        result += static_cast<char>(0x80 | (code & 63));
      } else {
        result += static_cast<char>(0xE0 | (code >> 12));
        result += static_cast<char>(0x80 | ((code >> 6) & 63));
        result += static_cast<char>(0x80 | (code & 63));
      }
    } else
      result += c;
  }
  if (result.starts_with("@@"))
    result.erase(0, 1);
  return result;
}
#ifdef _WIN32
fs::path known_folder(REFKNOWNFOLDERID id) {
  PWSTR text = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &text)))
    throw std::runtime_error("Cannot locate the previous launcher folders");
  fs::path result(text);
  CoTaskMemFree(text);
  return result;
}
std::optional<Json> registry_value(const std::string &key) {
  const auto slash = key.rfind('/');
  auto subkey = std::string("Software/Project Xenon/Xenon Launcher/") + key.substr(0, slash);
  std::replace(subkey.begin(), subkey.end(), '/', '\\');
  const auto wide_key = fs::u8path(subkey).wstring();
  const auto wide_name = fs::u8path(key.substr(slash + 1)).wstring();
  HKEY handle = nullptr;
  const auto opened =
      RegOpenKeyExW(HKEY_CURRENT_USER, wide_key.c_str(), 0, KEY_QUERY_VALUE, &handle);
  if (opened == ERROR_FILE_NOT_FOUND)
    return {};
  if (opened != ERROR_SUCCESS)
    throw std::runtime_error("The previous launcher settings could not be read");
  struct Close {
    HKEY key;
    ~Close() { RegCloseKey(key); }
  } close{handle};
  DWORD type = 0, bytes = 0;
  auto status = RegQueryValueExW(handle, wide_name.c_str(), nullptr, &type, nullptr, &bytes);
  if (status == ERROR_FILE_NOT_FOUND)
    return {};
  if (status != ERROR_SUCCESS || bytes > 16 * 1024 * 1024)
    throw std::runtime_error("Invalid previous launcher setting");
  std::vector<unsigned char> buffer(bytes + sizeof(wchar_t), 0);
  if (RegQueryValueExW(handle, wide_name.c_str(), nullptr, &type, buffer.data(), &bytes) !=
      ERROR_SUCCESS)
    throw std::runtime_error("The previous launcher setting changed while being read");
  if (type == REG_DWORD && bytes == sizeof(DWORD)) {
    DWORD value;
    std::memcpy(&value, buffer.data(), sizeof(value));
    return Json(value);
  }
  if (type == REG_QWORD && bytes == sizeof(std::uint64_t)) {
    std::uint64_t value;
    std::memcpy(&value, buffer.data(), sizeof(value));
    return Json(value);
  }
  if ((type == REG_SZ || type == REG_EXPAND_SZ || type == REG_BINARY) && bytes % 2 == 0) {
    auto count = bytes / sizeof(wchar_t);
    const auto *characters = reinterpret_cast<const wchar_t *>(buffer.data());
    while (count && characters[count - 1] == L'\0')
      --count;
    auto result = utf8(fs::path(std::wstring(characters, count)));
    if (result.starts_with("@@"))
      result.erase(0, 1);
    if (result.starts_with("@Variant(") || result.starts_with("@ByteArray("))
      return {};
    return Json(result);
  }
  return {};
}
#endif
} // namespace
Json read_legacy_ini(const fs::path &path) {
  if (!fs::exists(path))
    return Json::object();
  if (!fs::is_regular_file(path) || fs::file_size(path) > 16 * 1024 * 1024)
    throw std::runtime_error("Invalid legacy settings file");
  std::ifstream file(path);
  std::string line, group;
  Json result = Json::object();
  while (std::getline(file, line)) {
    line = trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#')
      continue;
    if (line.front() == '[' && line.back() == ']') {
      group = line.substr(1, line.size() - 2);
      if (group == "General")
        group.clear();
      continue;
    }
    const auto equals = line.find('=');
    if (equals == std::string::npos)
      continue;
    auto key = trim(line.substr(0, equals));
    std::replace(key.begin(), key.end(), '\\', '/');
    if (!group.empty())
      key = group + "/" + key;
    result[key] = unquote(line.substr(equals + 1));
  }
  return result;
}
std::optional<Json> LegacyStorage::value(const std::string &key) const {
  if (values.contains(key))
    return values.at(key);
#ifdef _WIN32
  if (registry)
    return registry_value(key);
#endif
  return {};
}
LegacyStorage legacy_storage(const fs::path &folder) {
  LegacyStorage result;
  if (!folder.empty()) {
    result.app = fs::weakly_canonical(fs::absolute(folder));
    if (!fs::is_directory(result.app))
      throw std::runtime_error("Choose a copied legacy launcher data folder");
    if (fs::exists(result.app / "settings.json"))
      result.values = read_document(result.app / "settings.json");
    else if (fs::exists(result.app / "settings.ini"))
      result.values = read_legacy_ini(result.app / "settings.ini");
    result.library = result.app;
  } else {
#ifdef _WIN32
    result.app = known_folder(FOLDERID_RoamingAppData) / "Project Xenon" / "Xenon Launcher";
    result.library = known_folder(FOLDERID_Documents) / "Xenon Launcher";
    result.screenshots = known_folder(FOLDERID_Pictures) / "Xenon";
    result.registry = true;
#else
    const auto *home = std::getenv("HOME");
    if (!home)
      throw std::runtime_error("Cannot locate the previous launcher data");
#ifdef __APPLE__
    throw std::runtime_error("Automatic macOS settings discovery is pending. Choose a copied "
                             "legacy data folder instead.");
#else
    const auto *data = std::getenv("XDG_DATA_HOME"), *config = std::getenv("XDG_CONFIG_HOME");
    result.app = (data ? fs::u8path(data) : fs::u8path(home) / ".local/share") / "Project Xenon" /
                 "Xenon Launcher";
    result.values = read_legacy_ini((config ? fs::u8path(config) : fs::u8path(home) / ".config") /
                                    "Project Xenon" / "Xenon Launcher.conf");
    result.library = fs::u8path(home) / "Documents" / "Xenon Launcher";
    result.screenshots = fs::u8path(home) / "Pictures" / "Xenon";
#endif
#endif
  }
  if (!result.values.is_object())
    throw std::runtime_error("Legacy settings must be a flat JSON object");
  const auto path = [&](const char *key, fs::path fallback) {
    const auto v = result.value(std::string("frontend/paths/") + key);
    return v && v->is_string() && !v->get<std::string>().empty() ? fs::u8path(v->get<std::string>())
                                                                 : fallback;
  };
  result.library = path("libraryRoot", result.library);
  result.profiles = path("profiles", result.library / "Profiles");
  result.games = path("games", result.library / "Games");
  result.saves = path("saves", result.library / "Saves");
  result.modules = path("modules", result.app / "Modules");
  result.screenshots = path("screenshots", result.screenshots.empty() ? result.app / "Captures"
                                                                      : result.screenshots);
  return result;
}
fs::path legacy_local_file(const std::string &value) {
  auto text = value;
  if (text.starts_with("file:///")) {
    text = text.substr(7);
    std::string decoded;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '%' && i + 2 < text.size() &&
          std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
          std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
        const auto byte = std::stoul(text.substr(i + 1, 2), nullptr, 16);
        if (!byte)
          throw std::runtime_error("Invalid legacy image URL");
        decoded += static_cast<char>(byte);
        i += 2;
      } else
        decoded += text[i];
    }
    text = decoded;
#ifdef _WIN32
    if (text.size() > 2 && text[0] == '/' && text[2] == ':')
      text.erase(0, 1);
#endif
  } else if (text.find("://") != std::string::npos)
    throw std::runtime_error("Only local legacy images can be migrated");
  return fs::u8path(text);
}
} // namespace xenon::launcher_next
