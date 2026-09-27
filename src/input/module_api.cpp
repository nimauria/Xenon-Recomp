#include "xenon/input/module_api_provider.hpp"

#include "xenon/input/system.hpp"

#include <algorithm>
#include <cstring>

namespace xenon::input::module_api {
namespace {

template <std::size_t N>
void copy_string(const std::string& value, char (&destination)[N]) {
  static_assert(N > 0);
  const auto count = std::min(value.size(), N - 1);
  std::memcpy(destination, value.data(), count);
  destination[count] = '\0';
}

[[nodiscard]] InputSystem* system_from(void* context) noexcept {
  return static_cast<InputSystem*>(context);
}

[[nodiscard]] std::int32_t result(Result value) noexcept {
  switch (value) {
    case Result::Success: return static_cast<std::int32_t>(ResultV1::Success);
    case Result::DeviceNotConnected:
      return static_cast<std::int32_t>(ResultV1::DeviceNotConnected);
    case Result::BadArguments: return static_cast<std::int32_t>(ResultV1::BadArguments);
    case Result::Empty: return static_cast<std::int32_t>(ResultV1::Empty);
    case Result::Unsupported: return static_cast<std::int32_t>(ResultV1::Unsupported);
    case Result::Failed: return static_cast<std::int32_t>(ResultV1::Failed);
  }
  return static_cast<std::int32_t>(ResultV1::Failed);
}

void copy_gamepad(const GamepadState& source, GamepadStateV1& destination) noexcept {
  destination.buttons = source.buttons;
  destination.left_trigger = source.left_trigger;
  destination.right_trigger = source.right_trigger;
  destination.thumb_lx = source.thumb_lx;
  destination.thumb_ly = source.thumb_ly;
  destination.thumb_rx = source.thumb_rx;
  destination.thumb_ry = source.thumb_ry;
}

void copy_device(const DeviceInfo& source, DeviceInfoV1& destination) {
  destination = {};
  destination.id = source.id;
  destination.ordinal = source.ordinal;
  destination.type = static_cast<std::uint8_t>(source.type);
  destination.subtype = static_cast<std::uint8_t>(source.subtype);
  destination.connection = static_cast<std::uint8_t>(source.connection);
  destination.player_indicator = source.player_indicator;
  destination.vendor_id = source.vendor_id;
  destination.product_id = source.product_id;
  destination.product_version = source.product_version;
  if (source.connected) destination.flags |= 1u << 0;
  if (source.supports_vibration) destination.flags |= 1u << 1;
  if (source.supports_keystrokes) destination.flags |= 1u << 2;
  if (source.supports_power_info) destination.flags |= 1u << 3;
  if (source.supports_player_indicator) destination.flags |= 1u << 4;
  copy_string(source.name, destination.name);
  copy_string(source.identity_key, destination.identity_key);
}

std::int32_t get_state(void* context, std::uint32_t user, StateV1* out) {
  if (!context || !out) return static_cast<std::int32_t>(ResultV1::BadArguments);
  State state{};
  const auto status = system_from(context)->get_state(user, state);
  if (status != Result::Success) return result(status);
  out->packet_number = state.packet_number;
  copy_gamepad(state.gamepad, out->gamepad);
  return static_cast<std::int32_t>(ResultV1::Success);
}

std::int32_t get_capabilities(void* context, std::uint32_t user,
                              CapabilitiesV1* out) {
  if (!context || !out) return static_cast<std::int32_t>(ResultV1::BadArguments);
  Capabilities caps{};
  const auto status = system_from(context)->get_capabilities(user, 0, caps);
  if (status != Result::Success) return result(status);
  *out = {};
  out->type = static_cast<std::uint8_t>(caps.type);
  out->subtype = static_cast<std::uint8_t>(caps.subtype);
  out->flags = caps.flags;
  copy_gamepad(caps.gamepad, out->gamepad);
  out->left_motor_speed = caps.vibration.left_motor_speed;
  out->right_motor_speed = caps.vibration.right_motor_speed;
  return static_cast<std::int32_t>(ResultV1::Success);
}

std::int32_t set_vibration(void* context, std::uint32_t user,
                           std::uint16_t left, std::uint16_t right) {
  if (!context) return static_cast<std::int32_t>(ResultV1::BadArguments);
  return result(system_from(context)->set_vibration(user, {left, right}));
}

std::int32_t get_keystroke(void* context, std::uint32_t user,
                           std::uint32_t flags, KeystrokeV1* out) {
  if (!context || !out) return static_cast<std::int32_t>(ResultV1::BadArguments);
  Keystroke key{};
  const auto status = system_from(context)->get_keystroke(user, flags, key);
  if (status != Result::Success) return result(status);
  *out = {};
  out->virtual_key = key.virtual_key;
  out->unicode = static_cast<std::uint16_t>(key.unicode);
  out->flags = key.flags;
  out->user_index = key.user_index;
  out->hid_code = key.hid_code;
  return static_cast<std::int32_t>(ResultV1::Success);
}

std::int32_t get_power(void* context, std::uint32_t user, PowerInfoV1* out) {
  if (!context || !out) return static_cast<std::int32_t>(ResultV1::BadArguments);
  PowerInfo power{};
  const auto status = system_from(context)->get_power_info(user, power);
  if (status != Result::Success) return result(status);
  out->source = static_cast<std::uint8_t>(power.source);
  out->level = static_cast<std::uint8_t>(power.level);
  out->percentage = power.percentage;
  out->charging = power.charging ? 1u : 0u;
  return static_cast<std::int32_t>(ResultV1::Success);
}

std::int32_t get_primary_device(void* context, std::uint32_t user,
                                DeviceInfoV1* out) {
  if (!context || !out || user >= kMaxUsers)
    return static_cast<std::int32_t>(ResultV1::BadArguments);
  auto* system = system_from(context);
  const auto id = system->device_for_user(user);
  if (!id) return static_cast<std::int32_t>(ResultV1::DeviceNotConnected);
  const auto device = system->device(*id);
  if (!device || !device->connected)
    return static_cast<std::int32_t>(ResultV1::DeviceNotConnected);
  copy_device(*device, *out);
  return static_cast<std::int32_t>(ResultV1::Success);
}

std::uint32_t get_source_count(void* context, std::uint32_t user) {
  if (!context || user >= kMaxUsers) return 0;
  return static_cast<std::uint32_t>(system_from(context)->sources_for_user(user).size());
}

std::int32_t get_source_device(void* context, std::uint32_t user,
                               std::uint32_t index, DeviceInfoV1* out) {
  if (!context || !out || user >= kMaxUsers)
    return static_cast<std::int32_t>(ResultV1::BadArguments);
  auto* system = system_from(context);
  const auto sources = system->sources_for_user(user);
  if (index >= sources.size()) return static_cast<std::int32_t>(ResultV1::BadArguments);
  const auto device = system->device(sources[index]);
  if (!device || !device->connected)
    return static_cast<std::int32_t>(ResultV1::DeviceNotConnected);
  copy_device(*device, *out);
  return static_cast<std::int32_t>(ResultV1::Success);
}

}  // namespace

Provider::Provider(InputSystem& system) {
  api_.abi_version = kAbiVersion;
  api_.struct_size = sizeof(ApiV1);
  api_.feature_flags = FeatureState | FeatureCapabilities | FeatureVibration |
                       FeatureKeystrokes | FeaturePower | FeatureDeviceMetadata |
                       FeatureProfiles | FeatureMultiSource | FeatureFlightDevices;
  api_.context = &system;
  api_.get_state = &get_state;
  api_.get_capabilities = &get_capabilities;
  api_.set_vibration = &set_vibration;
  api_.get_keystroke = &get_keystroke;
  api_.get_power = &get_power;
  api_.get_primary_device = &get_primary_device;
  api_.get_source_count = &get_source_count;
  api_.get_source_device = &get_source_device;
}

}  // namespace xenon::input::module_api
