#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include "xenon/xam/types.hpp"

namespace xenon::xam {

// Notification types
enum class NotificationType : std::uint32_t {
  FriendOnline = 0x00000001,
  FriendOffline = 0x00000002,
  GameInvite = 0x00000003,
  AchievementUnlocked = 0x00000004,
  MessageReceived = 0x00000005,
  Generic = 0x000000FF
};

// Notification data
struct Notification {
  std::uint32_t id{0};
  NotificationType type{NotificationType::Generic};
  std::uint32_t user_index{0};
  std::string message{};
  std::uint64_t param{0};
};

// Manages system notifications (stubbed - logged only)
class NotificationManager {
 public:
  NotificationManager();
  ~NotificationManager() = default;

  NotificationManager(const NotificationManager&) = delete;
  NotificationManager& operator=(const NotificationManager&) = delete;

  void initialize();

  // Notification queue
  void post_notification(const Notification& notification);
  [[nodiscard]] std::optional<Notification> get_next_notification();
  [[nodiscard]] bool has_notifications() const { return !queue_.empty(); }
  void clear_notifications() { queue_.clear(); }

  // Listener management (stubbed)
  [[nodiscard]] std::uint32_t create_listener() { return next_listener_id_++; }
  void close_listener(std::uint32_t /*listener_id*/) { /* Stubbed */ }

 private:
  std::deque<Notification> queue_{};
  std::uint32_t next_listener_id_{1};
  std::uint32_t next_notification_id_{1};
  bool initialized_{false};
};

}  // namespace xenon::xam
