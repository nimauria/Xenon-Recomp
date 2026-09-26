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

void InputSystem::auto_assign_locked() {
  for (std::size_t user_index = 0; user_index < users_.size(); ++user_index) {
    if (!auto_assignment_[user_index]) continue;
    auto& user = users_[user_index];
    if (user != kInvalidDeviceId) continue;

    const DeviceRecord* best = nullptr;
    for (const auto& [id, record] : records_) {
      static_cast<void>(id);
      if (!record.info.connected || record.info.type != DeviceType::Gamepad ||
          std::find(users_.begin(), users_.end(), record.info.id) != users_.end()) {
        continue;
      }
      bool used_as_extra = false;
      for (const auto& extras : extra_sources_) {
        if (std::find(extras.begin(), extras.end(), record.info.id) !=
            extras.end()) {
          used_as_extra = true;
          break;
        }
      }
      if (used_as_extra) continue;
      if (!best || record.info.ordinal < best->info.ordinal) best = &record;
    }
    if (best) user = best->info.id;
  }
}

std::optional<DeviceId> InputSystem::connected_id_for_identity_locked(
    std::string_view identity) const {
  const auto known = identity_to_id_.find(std::string(identity));
  if (known == identity_to_id_.end()) return std::nullopt;
  const auto record = records_.find(known->second);
  if (record == records_.end() || !record->second.info.connected) {
    return std::nullopt;
  }
  return known->second;
}

void InputSystem::reconcile_user_routes_locked() {
  std::unordered_set<DeviceId> claimed_primaries;

  // Restore explicit routes first. A secondary source may also be another
  // user's primary while the preferred source is connected, but if it is
  // promoted after a disconnect it must become the sole primary owner.
  for (std::size_t user_index = 0; user_index < users_.size(); ++user_index) {
    if (auto_assignment_[user_index]) continue;
    auto& primary = users_[user_index];
    auto& extras = extra_sources_[user_index];

    std::vector<DeviceId> restored;
    restored.reserve(desired_source_identities_[user_index].size());
    for (const auto& identity : desired_source_identities_[user_index]) {
      const auto id = connected_id_for_identity_locked(identity);
      if (!id || std::find(restored.begin(), restored.end(), *id) !=
                     restored.end()) {
        continue;
      }
      restored.push_back(*id);
    }
    const auto primary_it = std::find_if(
        restored.begin(), restored.end(), [&](DeviceId id) {
          return !claimed_primaries.contains(id);
        });
    const auto next_primary = primary_it == restored.end()
                                  ? kInvalidDeviceId
                                  : *primary_it;
    std::vector<DeviceId> next_extras;
    if (next_primary != kInvalidDeviceId) {
      claimed_primaries.emplace(next_primary);
      next_extras.reserve(restored.size() - 1);
      for (const auto id : restored) {
        if (id != next_primary) next_extras.push_back(id);
      }
    }
    if (primary != next_primary || extras != next_extras) {
      merged_has_state_[user_index] = false;
    }
    primary = next_primary;
    extras = std::move(next_extras);
  }

  // Automatic users retain a still-connected controller unless an explicit
  // route claimed it above. auto_assign_locked() fills any slots cleared here.
  for (std::size_t user_index = 0; user_index < users_.size(); ++user_index) {
    if (!auto_assignment_[user_index]) continue;
    auto& primary = users_[user_index];
    auto& extras = extra_sources_[user_index];
    if (primary != kInvalidDeviceId) {
      const auto it = records_.find(primary);
      if (it == records_.end() || !it->second.info.connected ||
          claimed_primaries.contains(primary)) {
        primary = kInvalidDeviceId;
        merged_has_state_[user_index] = false;
      } else {
        claimed_primaries.emplace(primary);
      }
    }
    extras.erase(
        std::remove_if(extras.begin(), extras.end(), [&](DeviceId id) {
          const auto it = records_.find(id);
          return it == records_.end() || !it->second.info.connected;
        }),
        extras.end());
  }
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

std::optional<DeviceId> InputSystem::device_for_user(
    std::uint32_t user_index) const {
  if (user_index >= kMaxUsers) return std::nullopt;
  std::scoped_lock lock(mutex_);
  const auto id = users_[user_index];
  if (id == kInvalidDeviceId) return std::nullopt;
  return id;
}

Result InputSystem::assign_user(std::uint32_t user_index, DeviceId device_id) {
  if (user_index >= kMaxUsers || device_id == kInvalidDeviceId) {
    return Result::BadArguments;
  }
  const std::array<DeviceId, 1> route{device_id};
  return set_user_sources(user_index, route);
}

Result InputSystem::set_user_sources(
    std::uint32_t user_index, std::span<const DeviceId> device_ids) {
  if (user_index >= kMaxUsers) return Result::BadArguments;
  std::vector<std::string> identities;
  {
    std::scoped_lock lock(mutex_);
    identities.reserve(device_ids.size());
    for (const auto id : device_ids) {
      if (id == kInvalidDeviceId) return Result::BadArguments;
      const auto it = records_.find(id);
      if (it == records_.end() || !it->second.info.connected) {
        return Result::DeviceNotConnected;
      }
      if (std::find(identities.begin(), identities.end(),
                    it->second.info.identity_key) != identities.end()) {
        return Result::BadArguments;
      }
      identities.push_back(it->second.info.identity_key);
    }
  }
  return set_user_source_identities(user_index, std::move(identities));
}

Result InputSystem::set_user_source_identities(
    std::uint32_t user_index, std::vector<std::string> identity_keys) {
  if (user_index >= kMaxUsers) return Result::BadArguments;
  std::unordered_set<std::string> unique;
  for (const auto& identity : identity_keys) {
    if (identity.empty()) return Result::BadArguments;
    if (!unique.emplace(identity).second) return Result::BadArguments;
  }
  {
    std::scoped_lock lock(mutex_);
    if (!identity_keys.empty()) {
      const auto& requested_primary = identity_keys.front();
      for (std::size_t other = 0; other < users_.size(); ++other) {
        if (other == user_index) continue;
        bool displaced_primary = false;
        if (users_[other] != kInvalidDeviceId) {
          const auto existing = records_.find(users_[other]);
          if (existing != records_.end() &&
              existing->second.info.identity_key == requested_primary) {
            users_[other] = kInvalidDeviceId;
            merged_has_state_[other] = false;
            displaced_primary = true;
          }
        }
        if (displaced_primary && !auto_assignment_[other]) {
          auto& desired = desired_source_identities_[other];
          desired.erase(std::remove(desired.begin(), desired.end(),
                                    requested_primary),
                        desired.end());
        }
      }
    }
    auto_assignment_[user_index] = false;
    desired_source_identities_[user_index] = std::move(identity_keys);
    reconcile_user_routes_locked();
    auto_assign_locked();
    merged_has_state_[user_index] = false;
  }
  sync_player_indicators();
  return Result::Success;
}

Result InputSystem::clear_user(std::uint32_t user_index) {
  if (user_index >= kMaxUsers) return Result::BadArguments;
  return set_user_source_identities(user_index, {});
}

Result InputSystem::add_user_source(std::uint32_t user_index, DeviceId device_id) {
  if (user_index >= kMaxUsers || device_id == kInvalidDeviceId) return Result::BadArguments;
  std::vector<DeviceId> route;
  {
    std::scoped_lock lock(mutex_);
    const auto it = records_.find(device_id);
    if (it == records_.end() || !it->second.info.connected) return Result::DeviceNotConnected;
    if (users_[user_index] == device_id || std::find(
            extra_sources_[user_index].begin(),
            extra_sources_[user_index].end(), device_id) !=
            extra_sources_[user_index].end()) {
      return Result::Success;
    }
    if (users_[user_index] != kInvalidDeviceId) {
      route.push_back(users_[user_index]);
    }
    route.insert(route.end(), extra_sources_[user_index].begin(),
                 extra_sources_[user_index].end());
    route.push_back(device_id);
  }
  return set_user_sources(user_index, route);
}

Result InputSystem::remove_user_source(std::uint32_t user_index, DeviceId device_id) {
  if (user_index >= kMaxUsers || device_id == kInvalidDeviceId) return Result::BadArguments;
  std::vector<DeviceId> route;
  {
    std::scoped_lock lock(mutex_);
    if (users_[user_index] == device_id) return Result::BadArguments;
    const auto& list = extra_sources_[user_index];
    if (std::find(list.begin(), list.end(), device_id) == list.end()) {
      return Result::DeviceNotConnected;
    }
    if (users_[user_index] != kInvalidDeviceId) {
      route.push_back(users_[user_index]);
    }
    for (const auto id : list) {
      if (id != device_id) route.push_back(id);
    }
  }
  return set_user_sources(user_index, route);
}

std::vector<std::string> InputSystem::desired_sources_for_user(
    std::uint32_t user_index) const {
  if (user_index >= kMaxUsers) return {};
  std::scoped_lock lock(mutex_);
  return desired_source_identities_[user_index];
}

void InputSystem::set_auto_assignment(std::uint32_t user_index, bool enabled) {
  if (user_index >= kMaxUsers) return;
  {
    std::scoped_lock lock(mutex_);
    if (auto_assignment_[user_index] == enabled) return;
    auto_assignment_[user_index] = enabled;
    desired_source_identities_[user_index].clear();
    if (!enabled) {
      if (users_[user_index] != kInvalidDeviceId) {
        desired_source_identities_[user_index].push_back(
            records_.at(users_[user_index]).info.identity_key);
      }
      for (const auto id : extra_sources_[user_index]) {
        const auto it = records_.find(id);
        if (it != records_.end()) {
          desired_source_identities_[user_index].push_back(
              it->second.info.identity_key);
        }
      }
    } else {
      extra_sources_[user_index].clear();
      reconcile_user_routes_locked();
      auto_assign_locked();
    }
    merged_has_state_[user_index] = false;
  }
  sync_player_indicators();
}

bool InputSystem::auto_assignment(std::uint32_t user_index) const {
  if (user_index >= kMaxUsers) return false;
  std::scoped_lock lock(mutex_);
  return auto_assignment_[user_index];
}

std::vector<DeviceId> InputSystem::sources_for_user(std::uint32_t user_index) const {
  if (user_index >= kMaxUsers) return {};
  std::scoped_lock lock(mutex_);
  std::vector<DeviceId> result;
  if (users_[user_index] != kInvalidDeviceId) result.push_back(users_[user_index]);
  for (auto id : extra_sources_[user_index]) {
    const auto it = records_.find(id);
    if (it != records_.end() && it->second.info.connected) result.push_back(id);
  }
  return result;
}

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

std::optional<InputSystem::RoutedDevice> InputSystem::route_user_locked(
    std::uint32_t user_index) const {
  if (user_index >= kMaxUsers) return std::nullopt;
  const auto id = users_[user_index];
  if (id == kInvalidDeviceId) return std::nullopt;
  const auto it = records_.find(id);
  if (it == records_.end() || !it->second.info.connected ||
      it->second.driver_index >= drivers_.size()) {
    return std::nullopt;
  }
  return RoutedDevice{id, it->second.driver_index, it->second.native_id,
                      it->second.info.identity_key};
}

Result InputSystem::poll_state_source(std::uint32_t user_index,
                                      const RoutedDevice& route,
                                      InputDriver& driver,
                                      GamepadState& out_state) {
  out_state = {};
  const auto result = driver.get_state(route.native_id, out_state);
  {
    std::scoped_lock lock(mutex_);
    const auto it = records_.find(route.id);
    if (it != records_.end() && it->second.info.connected &&
        it->second.driver_index == route.driver_index &&
        it->second.native_id == route.native_id) {
      it->second.last_result = result;
      if (result != Result::Success) ++it->second.state_failures;
    }
  }
  if (result == Result::Success) {
    profiles_.apply(user_index, route.identity_key, out_state);
  }
  return result;
}

void InputSystem::write_merged_state_locked(std::uint32_t user_index,
                                            const GamepadState& merged,
                                            State& out_state) {
  if (!merged_has_state_[user_index] ||
      merged_last_state_[user_index] != merged) {
    merged_last_state_[user_index] = merged;
    merged_has_state_[user_index] = true;
    ++merged_packet_number_[user_index];
  }
  out_state.packet_number = merged_packet_number_[user_index];
  out_state.gamepad = merged;
}

Result InputSystem::get_state(std::uint32_t user_index, State& out_state) {
  out_state = {};
  if (user_index >= kMaxUsers) return Result::BadArguments;
  std::scoped_lock driver_lock(driver_mutex_);

  struct Source {
    RoutedDevice route;
    InputDriver* driver{};
  };

  bool enabled{};
  Source primary{};
  std::vector<Source> additional_sources;
  {
    std::scoped_lock lock(mutex_);
    enabled = effective_active_locked();

    const auto routed = route_user_locked(user_index);
    if (!routed) return Result::DeviceNotConnected;
    primary.route = *routed;
    primary.driver = drivers_[routed->driver_index].driver.get();
    ++records_.at(routed->id).state_polls;

    const auto& extras = extra_sources_[user_index];
    if (!extras.empty()) {
      additional_sources.reserve(extras.size());
      for (const auto id : extras) {
        const auto it = records_.find(id);
        if (it == records_.end() || !it->second.info.connected ||
            it->second.driver_index >= drivers_.size()) {
          continue;
        }
        auto& record = it->second;
        ++record.state_polls;
        additional_sources.push_back(
            {{id, record.driver_index, record.native_id,
              record.info.identity_key},
             drivers_[record.driver_index].driver.get()});
      }
    }
  }

  GamepadState merged{};
  if (!enabled) {
    std::scoped_lock lock(mutex_);
    write_merged_state_locked(user_index, merged, out_state);
    return Result::Success;
  }

  bool any_success = false;
  Result first_error = Result::DeviceNotConnected;
  auto poll = [&](Source& source) {
    GamepadState state{};
    const auto result =
        poll_state_source(user_index, source.route, *source.driver, state);
    if (result == Result::Success) {
      merge_gamepad_state(merged, state);
      any_success = true;
    } else if (first_error == Result::DeviceNotConnected) {
      first_error = result;
    }
  };

  poll(primary);
  for (auto& source : additional_sources) poll(source);
  if (!any_success) return first_error;

  std::scoped_lock lock(mutex_);
  write_merged_state_locked(user_index, merged, out_state);
  return Result::Success;
}

Result InputSystem::get_capabilities(std::uint32_t user_index,
                                     std::uint32_t flags,
                                     Capabilities& out_caps) {
  out_caps = {};
  std::scoped_lock driver_lock(driver_mutex_);
  RoutedDevice route{};
  InputDriver* driver = nullptr;
  {
    std::scoped_lock lock(mutex_);
    const auto routed = route_user_locked(user_index);
    if (!routed) return user_index >= kMaxUsers ? Result::BadArguments
                                               : Result::DeviceNotConnected;
    route = *routed;
    driver = drivers_[route.driver_index].driver.get();
    ++records_.at(route.id).capability_queries;
  }
  static_cast<void>(flags);
  const auto result = driver->get_capabilities(route.native_id, out_caps);
  std::scoped_lock lock(mutex_);
  const auto it = records_.find(route.id);
  if (it != records_.end() && it->second.info.connected &&
      it->second.driver_index == route.driver_index &&
      it->second.native_id == route.native_id) {
    if (result != Result::Success) ++it->second.capability_failures;
    it->second.last_result = result;
  }
  return result;
}

Result InputSystem::set_vibration(std::uint32_t user_index,
                                  const Vibration& vibration) {
  std::scoped_lock driver_lock(driver_mutex_);
  RoutedDevice route{};
  InputDriver* driver = nullptr;
  {
    std::scoped_lock lock(mutex_);
    const auto routed = route_user_locked(user_index);
    if (!routed) return user_index >= kMaxUsers ? Result::BadArguments
                                               : Result::DeviceNotConnected;
    route = *routed;
    driver = drivers_[route.driver_index].driver.get();
    auto& record = records_.at(route.id);
    ++record.vibration_requests;
    if (!vibration_enabled_ || !effective_active_locked()) {
      record.last_result = Result::Success;
      return Result::Success;
    }
    if (record.last_vibration == vibration) {
      record.last_result = Result::Success;
      return Result::Success;
    }
  }

  const auto result = driver->set_vibration(route.native_id, vibration);
  std::scoped_lock lock(mutex_);
  const auto it = records_.find(route.id);
  if (it != records_.end() && it->second.info.connected &&
      it->second.driver_index == route.driver_index &&
      it->second.native_id == route.native_id) {
    if (result == Result::Success) {
      it->second.last_vibration = vibration;
    } else {
      ++it->second.vibration_failures;
    }
    it->second.last_result = result;
  }
  return result;
}

Result InputSystem::get_keystroke(std::uint32_t user_index,
                                  std::uint32_t flags,
  Keystroke& out_keystroke) {
  out_keystroke = {};
  static_cast<void>(flags);
  if (user_index == kAnyUser) {
    bool any_connected = false;
    for (std::uint32_t user = 0; user < kMaxUsers; ++user) {
      auto result = get_keystroke(user, flags, out_keystroke);
      if (result == Result::Success) return result;
      if (result != Result::DeviceNotConnected) any_connected = true;
      if (result != Result::Empty && result != Result::DeviceNotConnected) return result;
    }
    return any_connected ? Result::Empty : Result::DeviceNotConnected;
  }
  if (user_index >= kMaxUsers) return Result::BadArguments;
  std::scoped_lock driver_lock(driver_mutex_);

  struct Source { RoutedDevice route; InputDriver* driver{}; };
  std::vector<Source> sources;
  {
    std::scoped_lock lock(mutex_);
    if (!effective_active_locked()) return Result::Empty;
    sources.reserve(1 + extra_sources_[user_index].size());
    auto append = [&](DeviceId id) {
      if (id == kInvalidDeviceId) return;
      const auto it=records_.find(id);
      if(it==records_.end()||!it->second.info.connected||it->second.driver_index>=drivers_.size()) return;
      auto& r=it->second; ++r.keystroke_queries;
      sources.push_back({RoutedDevice{id,r.driver_index,r.native_id,r.info.identity_key},drivers_[r.driver_index].driver.get()});
    };
    append(users_[user_index]);
    for(auto id:extra_sources_[user_index]) append(id);
  }
  if (sources.empty()) return Result::DeviceNotConnected;

  bool any_connected = false;
  for (auto& source : sources) {
    Keystroke key{};
    const auto result =
        source.driver->get_keystroke(source.route.native_id, key);
    {
      std::scoped_lock lock(mutex_);
      const auto it = records_.find(source.route.id);
      if (it != records_.end() && it->second.info.connected &&
          it->second.driver_index == source.route.driver_index &&
          it->second.native_id == source.route.native_id) {
        if (result != Result::Success && result != Result::Empty &&
            result != Result::DeviceNotConnected) {
          ++it->second.keystroke_failures;
        }
        it->second.last_result = result;
      }
    }
    if (result == Result::Success) {
      key.user_index = static_cast<std::uint8_t>(user_index);
      out_keystroke = key;
      return Result::Success;
    }
    if (result != Result::DeviceNotConnected) any_connected = true;
    if (result != Result::Empty && result != Result::DeviceNotConnected) {
      return result;
    }
  }
  return any_connected ? Result::Empty : Result::DeviceNotConnected;
}

Result InputSystem::get_power_info(std::uint32_t user_index,
                                   PowerInfo& out_power) {
  out_power = {};
  std::scoped_lock driver_lock(driver_mutex_);
  RoutedDevice route{};
  InputDriver* driver = nullptr;
  {
    std::scoped_lock lock(mutex_);
    const auto routed = route_user_locked(user_index);
    if (!routed) return user_index >= kMaxUsers ? Result::BadArguments
                                               : Result::DeviceNotConnected;
    route = *routed;
    auto& record = records_.at(route.id);
    ++record.power_queries;
    if (!record.info.supports_power_info) {
      record.last_result = Result::Unsupported;
      return Result::Unsupported;
    }
    driver = drivers_[route.driver_index].driver.get();
  }

  const auto result = driver->get_power_info(route.native_id, out_power);
  std::scoped_lock lock(mutex_);
  const auto it = records_.find(route.id);
  if (it != records_.end() && it->second.info.connected &&
      it->second.driver_index == route.driver_index &&
      it->second.native_id == route.native_id) {
    if (result == Result::Success) {
      it->second.info.power = out_power;
      it->second.info.power_valid = true;
      if (out_power.source == PowerSource::Wired) {
        it->second.info.connection = ConnectionType::Wired;
      } else if (out_power.source == PowerSource::Battery) {
        it->second.info.connection = ConnectionType::Wireless;
      }
    } else {
      if (result != Result::Unsupported) ++it->second.power_failures;
      it->second.info.power_valid = false;
    }
    it->second.last_result = result;
  }
  return result;
}

Result InputSystem::set_player_indicator(std::uint32_t user_index,
                                         std::uint8_t player_index) {
  std::scoped_lock driver_lock(driver_mutex_);
  if (player_index != kNoPlayerIndicator && player_index >= kMaxUsers) {
    return Result::BadArguments;
  }
  RoutedDevice route{};
  InputDriver* driver = nullptr;
  {
    std::scoped_lock lock(mutex_);
    const auto routed = route_user_locked(user_index);
    if (!routed) return user_index >= kMaxUsers ? Result::BadArguments
                                               : Result::DeviceNotConnected;
    route = *routed;
    auto& record = records_.at(route.id);
    if (!record.info.supports_player_indicator) return Result::Unsupported;
    driver = drivers_[route.driver_index].driver.get();
  }

  const auto result = driver->set_player_indicator(route.native_id, player_index);
  if (result == Result::Success) {
    std::scoped_lock lock(mutex_);
    const auto it = records_.find(route.id);
    if (it != records_.end() && it->second.info.connected &&
        it->second.driver_index == route.driver_index &&
        it->second.native_id == route.native_id) {
      it->second.info.player_indicator = player_index;
    }
  }
  return result;
}

Result InputSystem::get_motion_state(std::uint32_t user_index,
                                     MotionState& out_motion) {
  out_motion = {};
  std::scoped_lock driver_lock(driver_mutex_);
  RoutedDevice route{};
  InputDriver* driver = nullptr;
  {
    std::scoped_lock lock(mutex_);
    const auto routed = route_user_locked(user_index);
    if (!routed) return user_index >= kMaxUsers ? Result::BadArguments
                                               : Result::DeviceNotConnected;
    route = *routed;
    const auto& record = records_.at(route.id);
    if (!record.info.supports_motion) return Result::Unsupported;
    driver = drivers_[route.driver_index].driver.get();
  }
  return driver->get_motion_state(route.native_id, out_motion);
}

Result InputSystem::get_touchpad_state(std::uint32_t user_index,
                                       TouchpadState& out_touch) {
  out_touch = {};
  std::scoped_lock driver_lock(driver_mutex_);
  RoutedDevice route{};
  InputDriver* driver = nullptr;
  {
    std::scoped_lock lock(mutex_);
    const auto routed = route_user_locked(user_index);
    if (!routed) return user_index >= kMaxUsers ? Result::BadArguments
                                               : Result::DeviceNotConnected;
    route = *routed;
    const auto& record = records_.at(route.id);
    if (!record.info.supports_touchpad) return Result::Unsupported;
    driver = drivers_[route.driver_index].driver.get();
  }
  return driver->get_touchpad_state(route.native_id, out_touch);
}

Result InputSystem::set_light_color(std::uint32_t user_index,
                                    const LightColor& color) {
  std::scoped_lock driver_lock(driver_mutex_);
  RoutedDevice route{};
  InputDriver* driver = nullptr;
  {
    std::scoped_lock lock(mutex_);
    const auto routed = route_user_locked(user_index);
    if (!routed) return user_index >= kMaxUsers ? Result::BadArguments
                                               : Result::DeviceNotConnected;
    route = *routed;
    const auto& record = records_.at(route.id);
    if (!record.info.supports_light_color) return Result::Unsupported;
    driver = drivers_[route.driver_index].driver.get();
  }
  return driver->set_light_color(route.native_id, color);
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
