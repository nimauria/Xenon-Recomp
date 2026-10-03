#pragma once

#include <memory>

#include "xenon/xam/achievement_manager.hpp"
#include "xenon/xam/content_manager.hpp"
#include "xenon/xam/locale_manager.hpp"
#include "xenon/xam/notification_manager.hpp"
#include "xenon/xam/user_manager.hpp"

namespace xenon::core {
class ExportRegistry;
class XenonSession;
}

namespace xenon::xam {

// XAM subsystem coordinator
// Owns user management, profiles, locale, storage, content, saves, etc.
// Provides unified XAM export registration
class XamSession {
 public:
  XamSession();
  ~XamSession();

  XamSession(const XamSession&) = delete;
  XamSession& operator=(const XamSession&) = delete;

  // Initialize XAM subsystem with default offline user
  [[nodiscard]] bool initialize();

  // Register all XAM exports into the export registry. `session` backs the
  // handful of exports that need the owning session's state (loaded XEX
  // execution-id header, guest virtual-memory allocator, title stop/relaunch)
  // - see xam_system_exports.cpp. Only read at call time (inside each export's
  // handler), never during this registration call itself, so it is safe to
  // call before a title is loaded.
  [[nodiscard]] bool register_exports(core::ExportRegistry& registry, core::XenonSession& session);

  // Subsystem access
  [[nodiscard]] UserManager& users() { return *user_manager_; }
  [[nodiscard]] const UserManager& users() const { return *user_manager_; }

  [[nodiscard]] LocaleManager& locale() { return *locale_manager_; }
  [[nodiscard]] const LocaleManager& locale() const { return *locale_manager_; }

  [[nodiscard]] ContentManager& content() { return *content_manager_; }
  [[nodiscard]] const ContentManager& content() const { return *content_manager_; }

  [[nodiscard]] NotificationManager& notifications() { return *notification_manager_; }
  [[nodiscard]] const NotificationManager& notifications() const { return *notification_manager_; }

  [[nodiscard]] AchievementManager& achievements() { return *achievement_manager_; }
  [[nodiscard]] const AchievementManager& achievements() const { return *achievement_manager_; }

 private:
  std::unique_ptr<UserManager> user_manager_;
  std::unique_ptr<LocaleManager> locale_manager_;
  std::unique_ptr<ContentManager> content_manager_;
  std::unique_ptr<NotificationManager> notification_manager_;
  std::unique_ptr<AchievementManager> achievement_manager_;
  bool initialized_{false};
};

}  // namespace xenon::xam
