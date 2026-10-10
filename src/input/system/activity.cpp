// Whether input reaches the guest: active, focus, background policy and
// vibration switches.

#include "xenon/input/system.hpp"
#include "xenon/input/state_merge.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace xenon::input {

bool InputSystem::effective_active_locked() const noexcept {
  return active_ &&
         (focused_ || background_policy_ == BackgroundInputPolicy::Always);
}

void InputSystem::stop_vibration_for_inactive_transition(bool was_active,
                                                         bool now_active) {
  if (!was_active || now_active) return;
  std::scoped_lock driver_lock(driver_mutex_);
  std::vector<std::pair<InputDriver*, NativeDeviceId>> stop_vibration;
  {
    std::scoped_lock lock(mutex_);
    for (auto& [id, record] : records_) {
      static_cast<void>(id);
      if (!record.info.connected || record.driver_index >= drivers_.size()) continue;
      if (record.last_vibration.left_motor_speed != 0 ||
          record.last_vibration.right_motor_speed != 0) {
        stop_vibration.emplace_back(drivers_[record.driver_index].driver.get(),
                                    record.native_id);
        record.last_vibration = {};
      }
    }
  }
  for (const auto& [driver, native] : stop_vibration) {
    if (driver) static_cast<void>(driver->set_vibration(native, {}));
  }
}

void InputSystem::set_active(bool active) {
  bool before{};
  bool after{};
  {
    std::scoped_lock lock(mutex_);
    before = effective_active_locked();
    active_ = active;
    after = effective_active_locked();
    if (before == after) return;
  }
  stop_vibration_for_inactive_transition(before, after);
}

bool InputSystem::active() const noexcept {
  std::scoped_lock lock(mutex_);
  return active_;
}

void InputSystem::set_focused(bool focused) {
  bool before{};
  bool after{};
  {
    std::scoped_lock lock(mutex_);
    before = effective_active_locked();
    focused_ = focused;
    after = effective_active_locked();
    if (before == after) return;
  }
  stop_vibration_for_inactive_transition(before, after);
}

bool InputSystem::focused() const noexcept {
  std::scoped_lock lock(mutex_);
  return focused_;
}

void InputSystem::set_background_input_policy(BackgroundInputPolicy policy) {
  bool before{};
  bool after{};
  {
    std::scoped_lock lock(mutex_);
    before = effective_active_locked();
    background_policy_ = policy;
    after = effective_active_locked();
    if (before == after) return;
  }
  stop_vibration_for_inactive_transition(before, after);
}

BackgroundInputPolicy InputSystem::background_input_policy() const noexcept {
  std::scoped_lock lock(mutex_);
  return background_policy_;
}

void InputSystem::set_vibration_enabled(bool enabled) {
  std::scoped_lock driver_lock(driver_mutex_);
  std::vector<std::pair<InputDriver*, NativeDeviceId>> stop_vibration;
  {
    std::scoped_lock lock(mutex_);
    if (vibration_enabled_ == enabled) return;
    vibration_enabled_ = enabled;
    if (!enabled) {
      for (auto& [id, record] : records_) {
        static_cast<void>(id);
        if (!record.info.connected || record.driver_index >= drivers_.size()) continue;
        if (record.last_vibration.left_motor_speed == 0 &&
            record.last_vibration.right_motor_speed == 0) {
          continue;
        }
        stop_vibration.emplace_back(drivers_[record.driver_index].driver.get(), record.native_id);
        record.last_vibration = {};
      }
    }
  }
  for (const auto& [driver, native] : stop_vibration) {
    if (driver) static_cast<void>(driver->set_vibration(native, {}));
  }
}

bool InputSystem::vibration_enabled() const noexcept {
  std::scoped_lock lock(mutex_);
  return vibration_enabled_;
}

bool InputSystem::effective_active() const noexcept {
  std::scoped_lock lock(mutex_);
  return effective_active_locked();
}

}  // namespace xenon::input
