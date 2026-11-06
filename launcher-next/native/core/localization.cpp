#include "localization.hpp"
#include <cstdlib>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
namespace xenon::launcher_next {
namespace {
std::string system_language() {
#ifdef _WIN32
  wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
  if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH))
    return {char(name[0]), char(name[1])};
#elif defined(__APPLE__)
  const auto languages = CFLocaleCopyPreferredLanguages();
  if (languages) {
    char name[128]{};
    const bool found =
        CFArrayGetCount(languages) > 0 &&
        CFStringGetCString(static_cast<CFStringRef>(CFArrayGetValueAtIndex(languages, 0)), name,
                           sizeof(name), kCFStringEncodingUTF8);
    CFRelease(languages);
    if (found)
      return name;
  }
#else
  for (const auto *key : {"LC_ALL", "LC_MESSAGES", "LANG"})
    if (const auto *value = std::getenv(key); value && *value)
      return value;
#endif
  return "en";
}
} // namespace
NativeCopy::NativeCopy(const std::filesystem::path &catalogue, const std::filesystem::path &state,
                       std::string system) {
  auto selected = system.empty() ? system_language() : std::move(system);
  try {
    const auto settings = read_document(state).at("settings");
    const auto value = settings.value("general/language", "auto");
    if (value != "auto")
      selected = value;
  } catch (const std::exception &) {
  }
  try {
    auto value = read_document(catalogue, 4 * 1024 * 1024);
    if (value.value("schema", "") != "xenon.localization" || value.at("version") != 1 ||
        !value.at("translations").is_object() || !value.at("translations").at("en").is_object())
      return;
    catalogue_ = std::move(value);
    for (auto &c : selected)
      if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
    const auto base = selected.substr(0, selected.find_first_of("-_."));
    if ((base == "en" || base == "fr" || base == "de" || base == "es") &&
        catalogue_.at("translations").contains(base) &&
        catalogue_.at("translations").at(base).is_object())
      language_ = base;
  } catch (const std::exception &) {
    catalogue_ = Json::object();
  }
}
std::string NativeCopy::text(const std::string &key, const std::string &fallback) const {
  if (!catalogue_.contains("translations"))
    return fallback;
  for (const auto &language : {language_, std::string("en")}) {
    const auto &values = catalogue_.at("translations").at(language);
    if (values.contains(key) && values.at(key).is_string()) {
      const auto value = values.at(key).get<std::string>();
      if (!value.empty() && value.size() <= 20000)
        return value;
    }
  }
  return fallback;
}
std::string NativeCopy::original(const std::string &english) const {
  std::string key;
  if (catalogue_.contains("sources") && catalogue_.at("sources").is_object())
    for (auto entry = catalogue_.at("sources").begin(); entry != catalogue_.at("sources").end();
         ++entry)
      if (entry.value().is_object() && entry.value().value("original", Json()) == english) {
        key = entry.key();
        if (!key.starts_with("native."))
          break;
      }
  return key.empty() ? english : text(key, english);
}
} // namespace xenon::launcher_next
