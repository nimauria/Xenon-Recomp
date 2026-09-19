#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xenon::input {

using DeviceId = std::uint64_t;
using NativeDeviceId = std::uint64_t;

constexpr DeviceId kInvalidDeviceId = 0;
constexpr std::uint32_t kMaxUsers = 4;
constexpr std::uint32_t kAnyUser = 0xFFFFFFFFu;

enum class Result : std::uint8_t {
  Success = 0,
  DeviceNotConnected,
  BadArguments,
  Empty,
  Unsupported,
  Failed,
};

[[nodiscard]] constexpr std::string_view to_string(Result result) noexcept {
  switch (result) {
    case Result::Success: return "success";
    case Result::DeviceNotConnected: return "device_not_connected";
    case Result::BadArguments: return "bad_arguments";
    case Result::Empty: return "empty";
    case Result::Unsupported: return "unsupported";
    case Result::Failed: return "failed";
  }
  return "unknown";
}

enum class DeviceType : std::uint8_t {
  Unknown = 0,
  Gamepad,
};

enum class DeviceSubtype : std::uint8_t {
  Unknown = 0,
  Gamepad,
  Wheel,
  ArcadeStick,
  FlightStick,
  DancePad,
  Guitar,
  DrumKit,
};

enum class ConnectionType : std::uint8_t {
  Unknown = 0,
  Wired,
  Wireless,
  Virtual,
};

// Power data is intentionally host-neutral. Percentage 0xFF means the host
// backend cannot provide a trustworthy numeric value.
enum class PowerSource : std::uint8_t {
  Unknown = 0,
  Wired,
  Battery,
};

enum class PowerLevel : std::uint8_t {
  Unknown = 0,
  Empty,
  Low,
  Medium,
  Full,
};

struct PowerInfo {
  PowerSource source{PowerSource::Unknown};
  PowerLevel level{PowerLevel::Unknown};
  std::uint8_t percentage{0xFFu};
  bool charging{};

  [[nodiscard]] bool operator==(const PowerInfo&) const noexcept = default;
};

constexpr std::uint8_t kNoPlayerIndicator = 0xFFu;

enum class BackgroundInputPolicy : std::uint8_t {
  ForegroundOnly = 0,
  Always,
};

enum GamepadButton : std::uint16_t {
  DpadUp = 0x0001,
  DpadDown = 0x0002,
  DpadLeft = 0x0004,
  DpadRight = 0x0008,
  Start = 0x0010,
  Back = 0x0020,
  LeftThumb = 0x0040,
  RightThumb = 0x0080,
  LeftShoulder = 0x0100,
  RightShoulder = 0x0200,
  Guide = 0x0400,
  A = 0x1000,
  B = 0x2000,
  X = 0x4000,
  Y = 0x8000,
};

struct GamepadState {
  std::uint16_t buttons{};
  std::uint8_t left_trigger{};
  std::uint8_t right_trigger{};
  std::int16_t thumb_lx{};
  std::int16_t thumb_ly{};
  std::int16_t thumb_rx{};
  std::int16_t thumb_ry{};

  [[nodiscard]] bool operator==(const GamepadState&) const noexcept = default;
};

struct State {
  std::uint32_t packet_number{};
  GamepadState gamepad{};
};

struct Vibration {
  std::uint16_t left_motor_speed{};
  std::uint16_t right_motor_speed{};

  [[nodiscard]] bool operator==(const Vibration&) const noexcept = default;
};

enum CapabilityFlag : std::uint16_t {
  CapabilityNone = 0,
  CapabilityVoiceSupported = 1u << 0u,
  CapabilityVibrationSupported = 1u << 1u,
  CapabilityKeystrokeSupported = 1u << 2u,
};

struct Capabilities {
  DeviceType type{DeviceType::Gamepad};
  DeviceSubtype subtype{DeviceSubtype::Gamepad};
  std::uint16_t flags{CapabilityNone};
  GamepadState gamepad{};
  Vibration vibration{};
};

enum KeystrokeFlag : std::uint16_t {
  KeystrokeKeyDown = 0x0001,
  KeystrokeKeyUp = 0x0002,
  KeystrokeRepeat = 0x0004,
};

struct Keystroke {
  std::uint16_t virtual_key{};
  char16_t unicode{};
  std::uint16_t flags{};
  std::uint8_t user_index{};
  std::uint8_t hid_code{};
};

// Driver-facing identity. persistent_key must remain the same if a physical
// device is unplugged and later reconnected. native_id only needs to remain
// valid while the device is connected.
struct DriverDeviceInfo {
  NativeDeviceId native_id{};
  std::string persistent_key{};
  std::string name{};
  DeviceType type{DeviceType::Gamepad};
  DeviceSubtype subtype{DeviceSubtype::Gamepad};
  ConnectionType connection{ConnectionType::Unknown};
  std::uint16_t vendor_id{};
  std::uint16_t product_id{};
  std::uint16_t product_version{};
  std::string serial{};
  std::string path{};
  bool supports_vibration{};
  bool supports_keystrokes{};
  bool supports_power_info{};
  bool supports_player_indicator{};
};

struct DeviceInfo {
  DeviceId id{kInvalidDeviceId};
  std::uint32_t ordinal{};
  std::string driver_name{};
  std::string persistent_key{};
  std::string identity_key{};
  std::string name{};
  DeviceType type{DeviceType::Gamepad};
  DeviceSubtype subtype{DeviceSubtype::Gamepad};
  ConnectionType connection{ConnectionType::Unknown};
  std::uint16_t vendor_id{};
  std::uint16_t product_id{};
  std::uint16_t product_version{};
  std::string serial{};
  std::string path{};
  bool supports_vibration{};
  bool supports_keystrokes{};
  bool supports_power_info{};
  bool supports_player_indicator{};
  PowerInfo power{};
  bool power_valid{};
  std::uint8_t player_indicator{kNoPlayerIndicator};
  bool connected{};
};

struct DeviceDiagnostics {
  DeviceId id{kInvalidDeviceId};
  std::uint32_t ordinal{};
  std::string identity_key{};
  bool connected{};
  std::int32_t assigned_user{-1};
  std::uint64_t state_polls{};
  std::uint64_t state_failures{};
  std::uint64_t capability_queries{};
  std::uint64_t capability_failures{};
  std::uint64_t vibration_requests{};
  std::uint64_t vibration_failures{};
  std::uint64_t keystroke_queries{};
  std::uint64_t keystroke_failures{};
  std::uint64_t power_queries{};
  std::uint64_t power_failures{};
  Result last_result{Result::Success};
};

struct InputDiagnostics {
  bool setup{};
  bool enabled{};
  bool focused{};
  bool effective_active{};
  BackgroundInputPolicy background_policy{BackgroundInputPolicy::ForegroundOnly};
  std::size_t driver_count{};
  std::size_t connected_device_count{};
  std::size_t assigned_user_count{};
  std::vector<DeviceDiagnostics> devices{};
};

}  // namespace xenon::input
