#include "xenon/xam/notification_manager.hpp"

#include <iostream>

namespace xenon::xam {

NotificationManager::NotificationManager() = default;

void NotificationManager::initialize() {
  if (initialized_) return;
  initialized_ = true;
}

void NotificationManager::post_notification(const Notification& notification) {
  // Log notification (since we don't display UI)
  std::cout << "[XAM Notification] Type=" << static_cast<std::uint32_t>(notification.type)
            << " User=" << notification.user_index
            << " Message=\"" << notification.message << "\""
            << std::endl;

  // Add to queue for games that poll
  Notification queued = notification;
  queued.id = next_notification_id_++;
  queue_.push_back(queued);

  // Keep queue bounded
  constexpr std::size_t kMaxQueueSize = 32;
  while (queue_.size() > kMaxQueueSize) {
    queue_.pop_front();
  }
}

std::optional<Notification> NotificationManager::get_next_notification() {
  if (queue_.empty()) {
    return std::nullopt;
  }

  Notification notification = queue_.front();
  queue_.pop_front();
  return notification;
}

}  // namespace xenon::xam
