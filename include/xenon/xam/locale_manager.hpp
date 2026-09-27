#pragma once

#include <cstdint>
#include <string>

#include "xenon/xam/types.hpp"

namespace xenon::xam {

// Manages locale, language, and regional settings
class LocaleManager {
 public:
  LocaleManager();
  ~LocaleManager() = default;

  LocaleManager(const LocaleManager&) = delete;
  LocaleManager& operator=(const LocaleManager&) = delete;

  // Initialize with default settings
  void initialize();

  // Language
  [[nodiscard]] LanguageId language() const { return language_; }
  void set_language(LanguageId lang) { language_ = lang; }

  // Locale (LCID - Windows locale identifier)
  [[nodiscard]] std::uint32_t locale() const { return locale_; }
  void set_locale(std::uint32_t lcid) { locale_ = lcid; }

  // Timezone information
  struct TimeZoneInfo {
    std::int32_t bias_minutes{0};           // Offset from UTC in minutes
    std::string standard_name{"UTC"};       // Standard timezone name
    std::string daylight_name{"UTC"};       // Daylight timezone name
    bool daylight_supported{false};         // DST support
  };

  [[nodiscard]] const TimeZoneInfo& timezone() const { return timezone_; }
  void set_timezone(const TimeZoneInfo& tz) { timezone_ = tz; }

 private:
  LanguageId language_{LanguageId::English};
  std::uint32_t locale_{0x0409};  // en-US by default
  TimeZoneInfo timezone_{};
  bool initialized_{false};
};

}  // namespace xenon::xam
