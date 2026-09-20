#include "xenon/xam/locale_manager.hpp"

namespace xenon::xam {

LocaleManager::LocaleManager() = default;

void LocaleManager::initialize() {
  if (initialized_) return;

  // Default to English/US
  language_ = LanguageId::English;
  locale_ = 0x0409;  // en-US LCID

  // Default timezone: UTC
  timezone_.bias_minutes = 0;
  timezone_.standard_name = "UTC";
  timezone_.daylight_name = "UTC";
  timezone_.daylight_supported = false;

  initialized_ = true;
}

}  // namespace xenon::xam
