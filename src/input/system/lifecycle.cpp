// InputSystem driver lifecycle and device discovery.

#include "xenon/input/system.hpp"
#include "xenon/input/state_merge.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace xenon::input {

InputSystem::~InputSystem() { shutdown(); }

bool InputSystem::add_driver(std::unique_ptr<InputDriver> driver) {
  if (!driver) return false;
  std::scoped_lock lock(mutex_);
  if (setup_) return false;
  const auto name = std::string(driver->name());
  drivers_.push_back({std::move(driver), name, false});
  return true;
}

Result InputSystem::setup() {
  std::vector<std::size_t> ready;
  {
    std::scoped_lock driver_lock(driver_mutex_);
    std::scoped_lock lock(mutex_);
    if (setup_) return Result::Success;
    setup_ = true;
    for (std::size_t i = 0; i < drivers_.size(); ++i) {
      const auto result = drivers_[i].driver->setup();
      if (result == Result::Success) {
        drivers_[i].setup = true;
        ready.push_back(i);
      }
    }
  }
  refresh_devices();
  return ready.empty() && !drivers_.empty() ? Result::Failed : Result::Success;
}

void InputSystem::shutdown() noexcept {
  std::scoped_lock driver_lock(driver_mutex_);
  std::vector<InputDriver*> drivers;
  std::vector<std::pair<InputDriver*, NativeDeviceId>> stop_vibration;
  {
    std::scoped_lock lock(mutex_);
    if (!setup_ && drivers_.empty()) return;
    for (auto& [id, record] : records_) {
      static_cast<void>(id);
      if (record.info.connected && record.driver_index < drivers_.size() &&
          (record.last_vibration.left_motor_speed != 0 ||
           record.last_vibration.right_motor_speed != 0)) {
        stop_vibration.emplace_back(drivers_[record.driver_index].driver.get(),
                                    record.native_id);
      }
      record.info.connected = false;
      record.info.player_indicator = kNoPlayerIndicator;
      record.has_state = false;
      record.last_vibration = {};
    }
    for (auto& slot : drivers_) {
      if (slot.setup && slot.driver) drivers.push_back(slot.driver.get());
      slot.setup = false;
    }
    users_.fill(kInvalidDeviceId);
    for (auto& sources : extra_sources_) sources.clear();
  }
  for (const auto& [driver, native] : stop_vibration) {
    if (driver) static_cast<void>(driver->set_vibration(native, {}));
  }
  for (auto* driver : drivers) driver->shutdown();
  {
    std::scoped_lock lock(mutex_);
    setup_ = false;
  }
}

std::string InputSystem::make_identity_key(std::string_view driver_name,
                                           std::string_view persistent_key) {
  std::string key;
  key.reserve(driver_name.size() + persistent_key.size() + 1);
  key.append(driver_name);
  key.push_back(':');
  key.append(persistent_key);
  return key;
}

void InputSystem::refresh_devices() {
  std::unique_lock driver_lock(driver_mutex_);
  struct Enumerated {
    std::size_t driver_index{};
    DriverDeviceInfo info{};
  };

  std::vector<Enumerated> enumerated;
  std::vector<DriverDeviceInfo> driver_devices;
  for (std::size_t i = 0; i < drivers_.size(); ++i) {
    if (!drivers_[i].setup || !drivers_[i].driver) continue;
    driver_devices.clear();
    drivers_[i].driver->enumerate_devices(driver_devices);
    enumerated.reserve(enumerated.size() + driver_devices.size());
    for (auto& device : driver_devices) {
      if (!device.persistent_key.empty()) {
        enumerated.push_back({i, std::move(device)});
      }
    }
  }

  {
    std::scoped_lock lock(mutex_);
    if (++refresh_generation_ == 0) {
      refresh_generation_ = 1;
      for (auto& [id, record] : records_) {
        static_cast<void>(id);
        record.refresh_generation = 0;
      }
    }

    for (auto& item : enumerated) {
      const auto& driver_name = drivers_[item.driver_index].name;
      const auto identity =
          make_identity_key(driver_name, item.info.persistent_key);

      DeviceId id = kInvalidDeviceId;
      if (const auto known = identity_to_id_.find(identity);
          known != identity_to_id_.end()) {
        id = known->second;
      } else {
        if (next_device_id_ == kInvalidDeviceId) ++next_device_id_;
        id = next_device_id_++;
        identity_to_id_.emplace(identity, id);

        DeviceRecord record{};
        record.info.id = id;
        record.info.ordinal = next_ordinal_++;
        record.info.driver_name = driver_name;
        record.info.persistent_key = item.info.persistent_key;
        record.info.identity_key = identity;
        records_.emplace(id, std::move(record));
      }

      auto& record = records_.at(id);
      record.driver_index = item.driver_index;
      record.native_id = item.info.native_id;
      record.refresh_generation = refresh_generation_;
      record.info.name = std::move(item.info.name);
      record.info.family = item.info.family;
      record.info.type = item.info.type;
      record.info.subtype = item.info.subtype;
      record.info.connection = item.info.connection;
      record.info.vendor_id = item.info.vendor_id;
      record.info.product_id = item.info.product_id;
      record.info.product_version = item.info.product_version;
      record.info.serial = std::move(item.info.serial);
      record.info.path = std::move(item.info.path);
      record.info.supports_vibration = item.info.supports_vibration;
      record.info.supports_keystrokes = item.info.supports_keystrokes;
      record.info.supports_power_info = item.info.supports_power_info;
      record.info.supports_player_indicator =
          item.info.supports_player_indicator;
      record.info.supports_motion = item.info.supports_motion;
      record.info.supports_touchpad = item.info.supports_touchpad;
      record.info.supports_light_color = item.info.supports_light_color;
      record.info.connected = true;
    }

    for (auto& [id, record] : records_) {
      static_cast<void>(id);
      if (record.refresh_generation == refresh_generation_) continue;
      record.info.connected = false;
      record.info.power_valid = false;
      record.info.player_indicator = kNoPlayerIndicator;
      record.has_state = false;
    }

    reconcile_user_routes_locked();
    auto_assign_locked();
  }
  driver_lock.unlock();
  sync_player_indicators();
}

void InputSystem::sync_player_indicators() {
  std::scoped_lock driver_lock(driver_mutex_);
  struct Update {
    DeviceId id{};
    InputDriver* driver{};
    NativeDeviceId native{};
    std::uint8_t player{kNoPlayerIndicator};
  };
  std::vector<Update> updates;
  {
    std::scoped_lock lock(mutex_);
    for (auto& [id, record] : records_) {
      if (!record.info.connected || !record.info.supports_player_indicator ||
          record.driver_index >= drivers_.size()) {
        continue;
      }
      std::uint8_t desired = kNoPlayerIndicator;
      const auto assigned = std::find(users_.begin(), users_.end(), id);
      if (assigned != users_.end()) {
        desired = static_cast<std::uint8_t>(
            std::distance(users_.begin(), assigned));
      }
      if (record.info.player_indicator == desired) continue;
      updates.push_back({id, drivers_[record.driver_index].driver.get(),
                         record.native_id, desired});
    }
  }

  for (const auto& update : updates) {
    const auto result = update.driver
                            ? update.driver->set_player_indicator(update.native,
                                                                  update.player)
                            : Result::Failed;
    if (result != Result::Success) continue;
    std::scoped_lock lock(mutex_);
    const auto it = records_.find(update.id);
    if (it != records_.end() && it->second.info.connected &&
        it->second.native_id == update.native) {
      it->second.info.player_indicator = update.player;
    }
  }
}

std::vector<DeviceInfo> InputSystem::devices() const {
  std::scoped_lock lock(mutex_);
  std::vector<DeviceInfo> result;
  result.reserve(records_.size());
  for (const auto& [id, record] : records_) {
    static_cast<void>(id);
    result.push_back(record.info);
  }
  std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.ordinal < rhs.ordinal;
  });
  return result;
}

std::optional<DeviceInfo> InputSystem::device(DeviceId id) const {
  std::scoped_lock lock(mutex_);
  const auto it = records_.find(id);
  if (it == records_.end()) return std::nullopt;
  return it->second.info;
}

InputDiagnostics InputSystem::diagnostics() const {
  std::scoped_lock lock(mutex_);
  InputDiagnostics result{};
  result.setup = setup_;
  result.enabled = active_;
  result.focused = focused_;
  result.effective_active = effective_active_locked();
  result.background_policy = background_policy_;
  result.driver_count = drivers_.size();
  for (const auto user : users_) {
    if (user != kInvalidDeviceId) ++result.assigned_user_count;
  }
  result.devices.reserve(records_.size());
  for (const auto& [id, record] : records_) {
    if (record.info.connected) ++result.connected_device_count;
    DeviceDiagnostics device{};
    device.id = id;
    device.ordinal = record.info.ordinal;
    device.identity_key = record.info.identity_key;
    device.connected = record.info.connected;
    const auto assigned = std::find(users_.begin(), users_.end(), id);
    if (assigned != users_.end()) {
      device.assigned_user = static_cast<std::int32_t>(
          std::distance(users_.begin(), assigned));
    } else {
      for (std::size_t user = 0; user < extra_sources_.size(); ++user) {
        if (std::find(extra_sources_[user].begin(), extra_sources_[user].end(), id) !=
            extra_sources_[user].end()) {
          device.assigned_user = static_cast<std::int32_t>(user);
          break;
        }
      }
    }
    device.state_polls = record.state_polls;
    device.state_failures = record.state_failures;
    device.capability_queries = record.capability_queries;
    device.capability_failures = record.capability_failures;
    device.vibration_requests = record.vibration_requests;
    device.vibration_failures = record.vibration_failures;
    device.keystroke_queries = record.keystroke_queries;
    device.keystroke_failures = record.keystroke_failures;
    device.power_queries = record.power_queries;
    device.power_failures = record.power_failures;
    device.last_result = record.last_result;
    result.devices.push_back(std::move(device));
  }
  std::sort(result.devices.begin(), result.devices.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.ordinal < rhs.ordinal;
            });
  return result;
}

}  // namespace xenon::input
