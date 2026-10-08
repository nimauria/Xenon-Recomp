// Per-user XInput-style queries: state, capabilities, keystrokes, power,
// motion and touchpad, plus vibration and indicator output.

#include "xenon/input/system.hpp"
#include "xenon/input/state_merge.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace xenon::input {

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

}  // namespace xenon::input
