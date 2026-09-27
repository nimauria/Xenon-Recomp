#include "xenon/input/sdl_driver.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace xenon::input {
namespace {

bool contains_case_insensitive(std::string_view value,
                               std::string_view needle) {
  if (needle.empty()) return true;
  return std::search(value.begin(), value.end(), needle.begin(), needle.end(),
                     [](char lhs, char rhs) {
                       return std::tolower(static_cast<unsigned char>(lhs)) ==
                              std::tolower(static_cast<unsigned char>(rhs));
                     }) != value.end();
}

ControllerFamily controller_family(SDL_GameController* controller) noexcept {
#if SDL_VERSION_ATLEAST(2, 0, 6)
  if (SDL_GameControllerGetVendor(controller) == 0x28DEu) {
    return ControllerFamily::Steam;
  }
#endif
  const char* name = SDL_GameControllerName(controller);
  if (name && (contains_case_insensitive(name, "steam") ||
               contains_case_insensitive(name, "valve"))) {
    return ControllerFamily::Steam;
  }
#if SDL_VERSION_ATLEAST(2, 0, 12)
  switch (SDL_GameControllerGetType(controller)) {
    case SDL_CONTROLLER_TYPE_XBOX360:
    case SDL_CONTROLLER_TYPE_XBOXONE:
      return ControllerFamily::Xbox;
    case SDL_CONTROLLER_TYPE_PS3:
    case SDL_CONTROLLER_TYPE_PS4:
#if SDL_VERSION_ATLEAST(2, 0, 14)
    case SDL_CONTROLLER_TYPE_PS5:
#endif
      return ControllerFamily::PlayStation;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 0, 14)
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
      return ControllerFamily::Nintendo;
    case SDL_CONTROLLER_TYPE_VIRTUAL:
      return ControllerFamily::Virtual;
    default:
      break;
  }
#endif
  // SDL2 predates a distinct Steam family, handled above using Valve's vendor
  // ID and the reported product name.
  return ControllerFamily::Generic;
}

class Sdl2Host final : public SdlHost {
 public:
  ~Sdl2Host() override { shutdown(); }

  Result setup() override {
    if (setup_) return Result::Success;
    SDL_version version{};
    SDL_GetVersion(&version);
    if (version.major < 2 ||
        (version.major == 2 && version.minor == 0 && version.patch < 9)) {
      return Result::Unsupported;
    }
    // Enable SDL's native HID paths before initializing the controller
    // subsystem. Environment/application hints still take precedence over
    // these normal-priority defaults.
#ifdef SDL_HINT_JOYSTICK_HIDAPI_PS5
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
#endif
#ifdef SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
#endif
#ifdef SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "1");
#endif
#ifdef SDL_HINT_JOYSTICK_HIDAPI_STEAM
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");
#endif
#ifdef SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "1");
#endif
    if (SDL_InitSubSystem(SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
      return Result::Failed;
    }
    setup_ = true;
    return Result::Success;
  }

  void shutdown() noexcept override {
    std::scoped_lock lock(mutex_);
    for (auto& [id, controller] : controllers_) {
      static_cast<void>(id);
      if (controller) SDL_GameControllerClose(controller);
    }
    controllers_.clear();
    if (setup_) {
      SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
      setup_ = false;
    }
  }

  int load_mappings_file(std::string_view path) override {
    if (!setup_ || path.empty()) return -1;
    const std::string owned(path);
    return SDL_GameControllerAddMappingsFromFile(owned.c_str());
  }

  Result enumerate(std::vector<SdlHostDevice>& out_devices) override {
    out_devices.clear();
    if (!setup_) return Result::Failed;
    std::scoped_lock lock(mutex_);
    SDL_GameControllerUpdate();

    std::unordered_set<SDL_JoystickID> seen;
    const int joystick_count = SDL_NumJoysticks();
    if (joystick_count < 0) return Result::Failed;
    for (int index = 0; index < joystick_count; ++index) {
      if (!SDL_IsGameController(index)) continue;
      const auto device_instance = SDL_JoystickGetDeviceInstanceID(index);
      if (device_instance < 0) continue;
      seen.insert(device_instance);
      if (!controllers_.contains(device_instance)) {
        auto* controller = SDL_GameControllerOpen(index);
        if (!controller) continue;
        auto* joystick = SDL_GameControllerGetJoystick(controller);
        if (!joystick) {
          SDL_GameControllerClose(controller);
          continue;
        }
        const auto actual_instance = SDL_JoystickInstanceID(joystick);
        if (actual_instance < 0) {
          SDL_GameControllerClose(controller);
          continue;
        }
        if (actual_instance != device_instance) {
          seen.erase(device_instance);
          seen.insert(actual_instance);
        }
        controllers_.emplace(actual_instance, controller);
      }
    }

    for (auto it = controllers_.begin(); it != controllers_.end();) {
      if (!seen.contains(it->first) || !SDL_GameControllerGetAttached(it->second)) {
        SDL_GameControllerClose(it->second);
        it = controllers_.erase(it);
      } else {
        ++it;
      }
    }

    struct RawDevice {
      SDL_JoystickID id{};
      SDL_GameController* controller{};
      std::string base_key{};
    };
    std::vector<RawDevice> raw;
    raw.reserve(controllers_.size());
    for (const auto& [id, controller] : controllers_) {
      raw.push_back({id, controller, make_base_key(controller)});
    }
    std::sort(raw.begin(), raw.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.base_key != rhs.base_key) return lhs.base_key < rhs.base_key;
      return lhs.id < rhs.id;
    });

    std::unordered_map<std::string, std::size_t> duplicate_index;
    for (const auto& item : raw) {
      SdlHostDevice device{};
      device.native_id = static_cast<NativeDeviceId>(
          static_cast<std::uint32_t>(item.id));
      device.persistent_key = item.base_key;
      const auto duplicate = duplicate_index[item.base_key]++;
      if (duplicate != 0) {
        device.persistent_key += "#" + std::to_string(duplicate);
      }
      const char* name = SDL_GameControllerName(item.controller);
      device.name = name ? name : "SDL Game Controller";
      device.family = controller_family(item.controller);
      device.subtype = DeviceSubtype::Gamepad;
      device.connection = connection_type(item.controller);
#if SDL_VERSION_ATLEAST(2, 0, 6)
      device.vendor_id = SDL_GameControllerGetVendor(item.controller);
      device.product_id = SDL_GameControllerGetProduct(item.controller);
#endif
#if SDL_VERSION_ATLEAST(2, 0, 12)
      device.product_version = SDL_GameControllerGetProductVersion(item.controller);
#endif
#if SDL_VERSION_ATLEAST(2, 0, 14)
      if (const char* serial = SDL_GameControllerGetSerial(item.controller);
          serial && *serial) {
        device.serial = serial;
      }
      device.supports_touchpad =
          SDL_GameControllerGetNumTouchpads(item.controller) > 0;
      const bool has_accelerometer = SDL_GameControllerHasSensor(
          item.controller, SDL_SENSOR_ACCEL) == SDL_TRUE;
      const bool has_gyroscope = SDL_GameControllerHasSensor(
          item.controller, SDL_SENSOR_GYRO) == SDL_TRUE;
      device.supports_motion = has_accelerometer || has_gyroscope;
      if (has_accelerometer) {
        SDL_GameControllerSetSensorEnabled(item.controller, SDL_SENSOR_ACCEL,
                                           SDL_TRUE);
      }
      if (has_gyroscope) {
        SDL_GameControllerSetSensorEnabled(item.controller, SDL_SENSOR_GYRO,
                                           SDL_TRUE);
      }
      device.supports_light_color =
          SDL_GameControllerHasLED(item.controller) == SDL_TRUE;
#endif
#if SDL_VERSION_ATLEAST(2, 24, 0)
      if (const char* path = SDL_GameControllerPath(item.controller);
          path && *path) {
        device.path = path;
      }
#endif
      device.supports_power_info = true;
#if SDL_VERSION_ATLEAST(2, 0, 12)
      device.supports_player_indicator = true;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 18)
      device.supports_vibration = SDL_GameControllerHasRumble(item.controller) == SDL_TRUE;
#else
      device.supports_vibration = true;
#endif
      out_devices.push_back(std::move(device));
    }
    return Result::Success;
  }

  Result get_state(NativeDeviceId device, GamepadState& out_state) override {
    out_state = {};
    if (!setup_) return Result::Failed;
    std::scoped_lock lock(mutex_);
    SDL_GameControllerUpdate();
    auto* controller = find_controller(device);
    if (!controller) return Result::DeviceNotConnected;

    auto button = [&](SDL_GameControllerButton value) {
      return SDL_GameControllerGetButton(controller, value) != 0;
    };
    if (button(SDL_CONTROLLER_BUTTON_DPAD_UP)) out_state.buttons |= DpadUp;
    if (button(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) out_state.buttons |= DpadDown;
    if (button(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) out_state.buttons |= DpadLeft;
    if (button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) out_state.buttons |= DpadRight;
    if (button(SDL_CONTROLLER_BUTTON_START)) out_state.buttons |= Start;
    if (button(SDL_CONTROLLER_BUTTON_BACK)) out_state.buttons |= Back;
    if (button(SDL_CONTROLLER_BUTTON_LEFTSTICK)) out_state.buttons |= LeftThumb;
    if (button(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) out_state.buttons |= RightThumb;
    if (button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) out_state.buttons |= LeftShoulder;
    if (button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) out_state.buttons |= RightShoulder;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (button(SDL_CONTROLLER_BUTTON_GUIDE)) out_state.buttons |= Guide;
#endif
    if (button(SDL_CONTROLLER_BUTTON_A)) out_state.buttons |= A;
    if (button(SDL_CONTROLLER_BUTTON_B)) out_state.buttons |= B;
    if (button(SDL_CONTROLLER_BUTTON_X)) out_state.buttons |= X;
    if (button(SDL_CONTROLLER_BUTTON_Y)) out_state.buttons |= Y;

    out_state.thumb_lx = axis(controller, SDL_CONTROLLER_AXIS_LEFTX, false);
    out_state.thumb_ly = axis(controller, SDL_CONTROLLER_AXIS_LEFTY, true);
    out_state.thumb_rx = axis(controller, SDL_CONTROLLER_AXIS_RIGHTX, false);
    out_state.thumb_ry = axis(controller, SDL_CONTROLLER_AXIS_RIGHTY, true);
    out_state.left_trigger = trigger(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    out_state.right_trigger = trigger(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    return Result::Success;
  }

  Result rumble(NativeDeviceId device, const Vibration& vibration,
                std::uint32_t duration_ms) override {
    if (!setup_) return Result::Failed;
    std::scoped_lock lock(mutex_);
    auto* controller = find_controller(device);
    if (!controller) return Result::DeviceNotConnected;
    const auto duration = static_cast<Uint32>(std::min<std::uint64_t>(
        duration_ms, std::numeric_limits<Uint32>::max()));
    return SDL_GameControllerRumble(controller, vibration.left_motor_speed,
                                    vibration.right_motor_speed, duration) == 0
               ? Result::Success
               : Result::Failed;
  }

  Result get_power_info(NativeDeviceId device, PowerInfo& out_power) override {
    out_power = {};
    if (!setup_) return Result::Failed;
    std::scoped_lock lock(mutex_);
    auto* controller = find_controller(device);
    if (!controller) return Result::DeviceNotConnected;
    auto* joystick = SDL_GameControllerGetJoystick(controller);
    if (!joystick) return Result::Failed;
    switch (SDL_JoystickCurrentPowerLevel(joystick)) {
      case SDL_JOYSTICK_POWER_EMPTY:
        out_power = {PowerSource::Battery, PowerLevel::Empty, 0xFFu, false};
        return Result::Success;
      case SDL_JOYSTICK_POWER_LOW:
        out_power = {PowerSource::Battery, PowerLevel::Low, 0xFFu, false};
        return Result::Success;
      case SDL_JOYSTICK_POWER_MEDIUM:
        out_power = {PowerSource::Battery, PowerLevel::Medium, 0xFFu, false};
        return Result::Success;
      case SDL_JOYSTICK_POWER_FULL:
        out_power = {PowerSource::Battery, PowerLevel::Full, 0xFFu, false};
        return Result::Success;
      case SDL_JOYSTICK_POWER_WIRED:
        out_power = {PowerSource::Wired, PowerLevel::Full, 0xFFu, false};
        return Result::Success;
      case SDL_JOYSTICK_POWER_UNKNOWN:
      default:
        out_power = {};
        return Result::Success;
    }
  }

  Result set_player_index(NativeDeviceId device,
                          std::uint8_t player_index) override {
#if SDL_VERSION_ATLEAST(2, 0, 12)
    if (!setup_) return Result::Failed;
    std::scoped_lock lock(mutex_);
    auto* controller = find_controller(device);
    if (!controller) return Result::DeviceNotConnected;
    const int index = player_index == kNoPlayerIndicator
                          ? -1
                          : static_cast<int>(player_index);
    SDL_GameControllerSetPlayerIndex(controller, index);
    return Result::Success;
#else
    static_cast<void>(device);
    static_cast<void>(player_index);
    return Result::Unsupported;
#endif
  }

  Result get_motion_state(NativeDeviceId device,
                          MotionState& out_motion) override {
    out_motion = {};
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (!setup_) return Result::Failed;
    std::scoped_lock lock(mutex_);
    auto* controller = find_controller(device);
    if (!controller) return Result::DeviceNotConnected;
    SDL_GameControllerUpdate();
    if (SDL_GameControllerHasSensor(controller, SDL_SENSOR_ACCEL) == SDL_TRUE) {
      if (SDL_GameControllerGetSensorData(controller, SDL_SENSOR_ACCEL,
                                          out_motion.acceleration.data(), 3) == 0) {
        out_motion.has_accelerometer = true;
      }
    }
    if (SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO) == SDL_TRUE) {
      if (SDL_GameControllerGetSensorData(
              controller, SDL_SENSOR_GYRO,
              out_motion.angular_velocity.data(), 3) == 0) {
        out_motion.has_gyroscope = true;
      }
    }
    return out_motion.has_accelerometer || out_motion.has_gyroscope
               ? Result::Success
               : Result::Unsupported;
#else
    static_cast<void>(device);
    return Result::Unsupported;
#endif
  }

  Result get_touchpad_state(NativeDeviceId device,
                            TouchpadState& out_touch) override {
    out_touch = {};
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (!setup_) return Result::Failed;
    std::scoped_lock lock(mutex_);
    auto* controller = find_controller(device);
    if (!controller) return Result::DeviceNotConnected;
    SDL_GameControllerUpdate();
    if (SDL_GameControllerGetNumTouchpads(controller) <= 0) {
      return Result::Unsupported;
    }
    const auto count = std::min<std::size_t>(
        kMaxTouchPoints,
        static_cast<std::size_t>(std::max(
            0, SDL_GameControllerGetNumTouchpadFingers(controller, 0))));
    for (std::size_t i = 0; i < count; ++i) {
      Uint8 down = 0;
      TouchPoint point{};
      if (SDL_GameControllerGetTouchpadFinger(
              controller, 0, static_cast<int>(i), &down, &point.x, &point.y,
              &point.pressure) != 0) {
        continue;
      }
      point.finger = static_cast<std::int32_t>(i);
      point.down = down != 0;
      out_touch.points[out_touch.point_count] = point;
      ++out_touch.point_count;
    }
    return Result::Success;
#else
    static_cast<void>(device);
    return Result::Unsupported;
#endif
  }

  Result set_light_color(NativeDeviceId device,
                         const LightColor& color) override {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (!setup_) return Result::Failed;
    std::scoped_lock lock(mutex_);
    auto* controller = find_controller(device);
    if (!controller) return Result::DeviceNotConnected;
    if (SDL_GameControllerHasLED(controller) != SDL_TRUE) {
      return Result::Unsupported;
    }
    return SDL_GameControllerSetLED(controller, color.red, color.green,
                                    color.blue) == 0
               ? Result::Success
               : Result::Failed;
#else
    static_cast<void>(device);
    static_cast<void>(color);
    return Result::Unsupported;
#endif
  }

  std::uint64_t now_millis() const noexcept override {
#if SDL_VERSION_ATLEAST(2, 0, 18)
    return SDL_GetTicks64();
#else
    return SDL_GetTicks();
#endif
  }

 private:
  SDL_GameController* find_controller(NativeDeviceId device) const noexcept {
    const auto id = static_cast<SDL_JoystickID>(
        static_cast<std::uint32_t>(device));
    const auto it = controllers_.find(id);
    return it == controllers_.end() ? nullptr : it->second;
  }

  static std::int16_t axis(SDL_GameController* controller,
                           SDL_GameControllerAxis which, bool invert) noexcept {
    const auto raw = static_cast<std::int32_t>(
        SDL_GameControllerGetAxis(controller, which));
    if (!invert) return static_cast<std::int16_t>(raw);
    const auto inverted = std::clamp(-raw, -32768, 32767);
    return static_cast<std::int16_t>(inverted);
  }

  static std::uint8_t trigger(SDL_GameController* controller,
                              SDL_GameControllerAxis which) noexcept {
    const auto raw = std::max<int>(0, SDL_GameControllerGetAxis(controller, which));
    return static_cast<std::uint8_t>(std::min(255, raw >> 7));
  }

  static ConnectionType connection_type(SDL_GameController* controller) noexcept {
    auto* joystick = SDL_GameControllerGetJoystick(controller);
    if (!joystick) return ConnectionType::Unknown;
    const auto power = SDL_JoystickCurrentPowerLevel(joystick);
    if (power >= SDL_JOYSTICK_POWER_EMPTY && power <= SDL_JOYSTICK_POWER_FULL) {
      return ConnectionType::Wireless;
    }
    if (power == SDL_JOYSTICK_POWER_WIRED) return ConnectionType::Wired;
    return ConnectionType::Unknown;
  }

  static std::string make_base_key(SDL_GameController* controller) {
    auto* joystick = SDL_GameControllerGetJoystick(controller);
    if (!joystick) return "unknown";
    char guid_text[33]{};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid_text,
                              static_cast<int>(sizeof(guid_text)));
    std::ostringstream key;
    key << guid_text;
#if SDL_VERSION_ATLEAST(2, 0, 6)
    key << ':' << std::hex << std::setw(4) << std::setfill('0')
        << SDL_GameControllerGetVendor(controller) << ':' << std::setw(4)
        << SDL_GameControllerGetProduct(controller);
#endif
#if SDL_VERSION_ATLEAST(2, 0, 12)
    key << ":v=" << std::setw(4)
        << SDL_GameControllerGetProductVersion(controller);
#endif
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (const char* serial = SDL_GameControllerGetSerial(controller);
        serial && *serial) {
      key << ":serial=" << serial;
    } else
#endif
    {
#if SDL_VERSION_ATLEAST(2, 24, 0)
      if (const char* path = SDL_GameControllerPath(controller); path && *path) {
        key << ":path=" << path;
        return key.str();
      }
#endif
      if (const char* name = SDL_GameControllerName(controller); name && *name) {
        key << ":name=" << name;
      }
    }
    return key.str();
  }

  bool setup_{};
  mutable std::mutex mutex_{};
  std::unordered_map<SDL_JoystickID, SDL_GameController*> controllers_{};
};

}  // namespace

std::unique_ptr<SdlHost> create_sdl2_host() {
  return std::make_unique<Sdl2Host>();
}

}  // namespace xenon::input
