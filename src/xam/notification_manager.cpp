#include "xenon/xam/notification_manager.hpp"

#include <algorithm>

#include "xenon/kernel/event.hpp"

namespace xenon::xam {

NotificationManager::NotificationManager() = default;
NotificationManager::~NotificationManager() = default;

void NotificationManager::initialize() {}

void NotificationManager::enqueue_locked(Listener& listener, std::uint32_t id,
                                         std::uint32_t param) {
  if ((listener.mask & (std::uint64_t{1} << notification::area(id))) == 0u) return;
  if (notification::version(id) > listener.max_version) return;
  auto event = listener.event.lock();
  if (!event) return;
  listener.queue.emplace_back(id, param);
  event->set();
}

std::shared_ptr<kernel::KernelEvent> NotificationManager::create_listener(
    std::uint64_t mask, std::uint32_t max_version) {
  auto event = std::make_shared<kernel::KernelEvent>(/*manual_reset=*/true, false);
  std::scoped_lock lock(mutex_);
  // Listeners whose handle has been closed are dropped here.
  listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                  [](const Listener& l) { return l.event.expired(); }),
                   listeners_.end());
  auto& listener = listeners_.emplace_back();
  listener.event = event;
  listener.key = event.get();
  listener.mask = mask;
  listener.max_version = std::min<std::uint32_t>(max_version, 10u);
  if (!notified_startup_ && (mask & 1u) != 0u) {
    notified_startup_ = true;
    enqueue_locked(listener, notification::kSystemUi, 1u);
    enqueue_locked(listener, notification::kSystemUi, 0u);
    enqueue_locked(listener, notification::kSigninChanged, 1u);
    enqueue_locked(listener, notification::kSigninChanged, 1u);
    enqueue_locked(listener, notification::kInputDevicesChanged, 0u);
    enqueue_locked(listener, notification::kInputDevicesChanged, 0u);
    enqueue_locked(listener, notification::kInputDeviceConfigChanged, 0u);
    enqueue_locked(listener, notification::kInputDeviceConfigChanged, 0u);
  }
  return event;
}

void NotificationManager::broadcast(std::uint32_t id, std::uint32_t param) {
  std::scoped_lock lock(mutex_);
  for (auto& listener : listeners_) enqueue_locked(listener, id, param);
}

bool NotificationManager::dequeue(const kernel::KernelObject* listener_object,
                                  std::uint32_t match_id, std::uint32_t& out_id,
                                  std::uint32_t& out_param) {
  std::scoped_lock lock(mutex_);
  const auto it = std::find_if(listeners_.begin(), listeners_.end(), [&](const Listener& l) {
    return l.key == listener_object && !l.event.expired();
  });
  if (it == listeners_.end()) return false;
  auto& queue = it->queue;
  const auto item = std::find_if(queue.begin(), queue.end(), [&](const auto& entry) {
    return match_id == 0u || entry.first == match_id;
  });
  if (item == queue.end()) return false;
  out_id = item->first;
  out_param = item->second;
  queue.erase(item);
  if (queue.empty()) {
    if (auto event = it->event.lock()) event->reset();
  }
  return true;
}

}  // namespace xenon::xam
