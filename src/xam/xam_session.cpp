#include "xenon/xam/xam_session.hpp"

#include "xenon/core/export_registry.hpp"
#include "xenon/xam/xam_user_exports.hpp"

// Forward declarations for export registration functions
namespace xenon::xam {
bool register_locale_exports(core::ExportRegistry& registry, LocaleManager& locale_manager);
bool register_content_exports(core::ExportRegistry& registry, ContentManager& content_manager);
bool register_notification_exports(core::ExportRegistry& registry, NotificationManager& notification_manager);
bool register_achievement_exports(core::ExportRegistry& registry, AchievementManager& achievement_manager);
}

namespace xenon::xam {

XamSession::XamSession() 
    : user_manager_(std::make_unique<UserManager>()),
      locale_manager_(std::make_unique<LocaleManager>()),
      content_manager_(std::make_unique<ContentManager>()),
      notification_manager_(std::make_unique<NotificationManager>()),
      achievement_manager_(std::make_unique<AchievementManager>()) {}

XamSession::~XamSession() = default;

bool XamSession::initialize() {
  if (initialized_) return true;

  user_manager_->initialize();
  locale_manager_->initialize();
  content_manager_->initialize();
  notification_manager_->initialize();
  achievement_manager_->initialize();
  
  initialized_ = true;
  return true;
}

bool XamSession::register_exports(core::ExportRegistry& registry) {
  bool ok = true;
  
  // Register all XAM export categories
  ok = register_user_exports(registry, *user_manager_) && ok;
  ok = register_locale_exports(registry, *locale_manager_) && ok;
  ok = register_content_exports(registry, *content_manager_) && ok;
  ok = register_notification_exports(registry, *notification_manager_) && ok;
  ok = register_achievement_exports(registry, *achievement_manager_) && ok;
  
  return ok;
}

}  // namespace xenon::xam
