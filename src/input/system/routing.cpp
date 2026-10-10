// Routing guest users to host input devices: explicit assignment, extra
// sources and automatic assignment.

#include "xenon/input/system.hpp"
#include "xenon/input/state_merge.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace xenon::input {

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

}  // namespace xenon::input
