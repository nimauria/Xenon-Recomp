#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace xenon::kernel {
class KernelEvent;
class KernelObject;
}  // namespace xenon::kernel

namespace xenon::xam {

// Notification ids as XAM defines them: local id in bits 0-15, version in
// bits 16-24 and area (the bit tested in a listener's mask) in bits 25-30,
// as in xenia's XNotificationKey.
namespace notification {
constexpr std::uint32_t kSystemUi = 0x00000009u;                  // XN_SYS_UI
constexpr std::uint32_t kSigninChanged = 0x0000000Au;             // XN_SYS_SIGNINCHANGED
constexpr std::uint32_t kStorageDevicesChanged = 0x0000000Bu;     // XN_SYS_STORAGEDEVICESCHANGED
constexpr std::uint32_t kProfileSettingChanged = 0x0000000Eu;     // XN_SYS_PROFILESETTINGCHANGED
constexpr std::uint32_t kInputDevicesChanged = 0x00000012u;       // XN_SYS_INPUTDEVICESCHANGED
constexpr std::uint32_t kInputDeviceConfigChanged = 0x00000013u;  // XN_SYS_INPUTDEVICECONFIGCHANGED
constexpr std::uint32_t kLiveConnectionChanged = 0x02000001u;     // XN_LIVE_CONNECTIONCHANGED

[[nodiscard]] constexpr std::uint32_t area(std::uint32_t id) noexcept { return (id >> 25u) & 0x3Fu; }
[[nodiscard]] constexpr std::uint32_t version(std::uint32_t id) noexcept { return (id >> 16u) & 0x1FFu; }
}  // namespace notification

// XNotify listeners (XamNotifyCreateListener, XNotifyGetNext). Each listener
// is a manual-reset kernel event, signalled while it has notifications
// queued, so a title can wait on the listener handle; its mask selects the
// areas it receives and max_version drops newer notification versions.
//
// The first listener created with the system area in its mask is given the
// notifications titles expect at startup - XN_SYS_UI on then off,
// XN_SYS_SIGNINCHANGED twice, XN_SYS_INPUTDEVICESCHANGED and
// XN_SYS_INPUTDEVICECONFIGCHANGED twice each - as xenia and rexglue-sdk do
// (KernelState::RegisterNotifyListener).
class NotificationManager {
 public:
  NotificationManager();
  ~NotificationManager();

  NotificationManager(const NotificationManager&) = delete;
  NotificationManager& operator=(const NotificationManager&) = delete;

  void initialize();

  [[nodiscard]] std::shared_ptr<kernel::KernelEvent> create_listener(std::uint64_t mask,
                                                                     std::uint32_t max_version);

  // Queues `id` with `param` for every listener that accepts it.
  void broadcast(std::uint32_t id, std::uint32_t param);

  // Takes the oldest notification queued for `listener`, or with
  // `match_id` non-zero the oldest with that id. False when `listener` is
  // not a listener or nothing matches.
  [[nodiscard]] bool dequeue(const kernel::KernelObject* listener, std::uint32_t match_id,
                             std::uint32_t& out_id, std::uint32_t& out_param);

 private:
  struct Listener {
    std::weak_ptr<kernel::KernelEvent> event;
    const kernel::KernelObject* key{nullptr};
    std::uint64_t mask{0};
    std::uint32_t max_version{0};
    std::deque<std::pair<std::uint32_t, std::uint32_t>> queue;
  };
  static void enqueue_locked(Listener& listener, std::uint32_t id, std::uint32_t param);

  std::mutex mutex_;
  std::vector<Listener> listeners_;
  bool notified_startup_{false};
};

}  // namespace xenon::xam
