#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace xenon::input::module_api {

// Native game-module ABI version. This is deliberately independent from the
// Xbox/XAM guest ABI and from InputSystem's private C++ implementation.
constexpr std::uint32_t kAbiVersion = 1;
constexpr std::uint32_t kMaxDeviceName = 96;
constexpr std::uint32_t kMaxIdentityKey = 192;
constexpr std::uint8_t kNoPlayerIndicatorV1 = 0xFFu;

enum Feature : std::uint64_t {
  FeatureState = 1ull << 0,
  FeatureCapabilities = 1ull << 1,
  FeatureVibration = 1ull << 2,
  FeatureKeystrokes = 1ull << 3,
  FeaturePower = 1ull << 4,
  FeatureDeviceMetadata = 1ull << 5,
  // Profiles are launcher/runtime-owned, but all states returned through this
  // API have the selected profile/calibration already applied.
  FeatureProfiles = 1ull << 6,
  FeatureMultiSource = 1ull << 7,
  FeatureFlightDevices = 1ull << 8,
};

// Stable result values for native modules. Do not expose the private Result
// enum directly: this table is an ABI contract and its values must not drift if
// Xenon's internal representation changes later.
enum class ResultV1 : std::int32_t {
  Success = 0,
  DeviceNotConnected = 1,
  BadArguments = 2,
  Empty = 3,
  Unsupported = 4,
  Failed = 5,
};

struct GamepadStateV1 {
  std::uint16_t buttons{};
  std::uint8_t left_trigger{};
  std::uint8_t right_trigger{};
  std::int16_t thumb_lx{};
  std::int16_t thumb_ly{};
  std::int16_t thumb_rx{};
  std::int16_t thumb_ry{};
};

struct StateV1 {
  std::uint32_t packet_number{};
  GamepadStateV1 gamepad{};
};

struct CapabilitiesV1 {
  std::uint8_t type{};
  std::uint8_t subtype{};
  std::uint16_t flags{};
  GamepadStateV1 gamepad{};
  std::uint16_t left_motor_speed{};
  std::uint16_t right_motor_speed{};
};

struct KeystrokeV1 {
  std::uint16_t virtual_key{};
  std::uint16_t unicode{};
  std::uint16_t flags{};
  std::uint8_t user_index{};
  std::uint8_t hid_code{};
};

struct PowerInfoV1 {
  std::uint8_t source{};
  std::uint8_t level{};
  std::uint8_t percentage{0xFFu};
  std::uint8_t charging{};
};

struct DeviceInfoV1 {
  std::uint64_t id{};
  std::uint32_t ordinal{};
  std::uint8_t type{};
  std::uint8_t subtype{};
  std::uint8_t connection{};
  std::uint8_t player_indicator{kNoPlayerIndicatorV1};
  std::uint16_t vendor_id{};
  std::uint16_t product_id{};
  std::uint16_t product_version{};
  std::uint16_t reserved{};
  std::uint32_t flags{};
  char name[kMaxDeviceName]{};
  char identity_key[kMaxIdentityKey]{};
};

// Versioned native function table supplied by the Xenon runtime to a game
// module that negotiated runtimeApis.input v1. Modules must check abi_version,
// struct_size and the relevant feature bit before using an optional function.
struct ApiV1 {
  std::uint32_t abi_version{kAbiVersion};
  std::uint32_t struct_size{};
  std::uint64_t feature_flags{};
  void* context{};

  std::int32_t (*get_state)(void*, std::uint32_t, StateV1*){};
  std::int32_t (*get_capabilities)(void*, std::uint32_t, CapabilitiesV1*){};
  std::int32_t (*set_vibration)(void*, std::uint32_t, std::uint16_t, std::uint16_t){};
  std::int32_t (*get_keystroke)(void*, std::uint32_t, std::uint32_t, KeystrokeV1*){};
  std::int32_t (*get_power)(void*, std::uint32_t, PowerInfoV1*){};
  std::int32_t (*get_primary_device)(void*, std::uint32_t, DeviceInfoV1*){};
  std::uint32_t (*get_source_count)(void*, std::uint32_t){};
  std::int32_t (*get_source_device)(void*, std::uint32_t, std::uint32_t, DeviceInfoV1*){};
};

static_assert(std::is_standard_layout_v<GamepadStateV1>);
static_assert(std::is_trivially_copyable_v<GamepadStateV1>);
static_assert(std::is_standard_layout_v<StateV1>);
static_assert(std::is_trivially_copyable_v<StateV1>);
static_assert(std::is_standard_layout_v<CapabilitiesV1>);
static_assert(std::is_trivially_copyable_v<CapabilitiesV1>);
static_assert(std::is_standard_layout_v<KeystrokeV1>);
static_assert(std::is_trivially_copyable_v<KeystrokeV1>);
static_assert(std::is_standard_layout_v<PowerInfoV1>);
static_assert(std::is_trivially_copyable_v<PowerInfoV1>);
static_assert(std::is_standard_layout_v<DeviceInfoV1>);
static_assert(std::is_trivially_copyable_v<DeviceInfoV1>);
static_assert(std::is_standard_layout_v<ApiV1>);
static_assert(std::is_trivially_copyable_v<ApiV1>);

[[nodiscard]] constexpr bool compatible_v1(const ApiV1* api) noexcept {
  return api != nullptr && api->abi_version == kAbiVersion &&
         api->struct_size >= sizeof(ApiV1);
}

[[nodiscard]] constexpr bool has_feature(const ApiV1* api, Feature feature) noexcept {
  return compatible_v1(api) &&
         (api->feature_flags & static_cast<std::uint64_t>(feature)) != 0;
}

}  // namespace xenon::input::module_api
