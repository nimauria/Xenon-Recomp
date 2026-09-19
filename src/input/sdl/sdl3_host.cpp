#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "xenon/input/sdl_driver.hpp"

namespace xenon::input {
namespace {

DeviceSubtype gamepad_subtype(SDL_GamepadType /*type*/) noexcept {
  // SDL's vendor-specific gamepad types all map to the standard Xbox gamepad
  // contract. Specialized Xbox subtypes are provided by dedicated backends.
  return DeviceSubtype::Gamepad;
}

std::int16_t invert_axis(std::int16_t value) noexcept {
  return static_cast<std::int16_t>(
      std::clamp(-static_cast<int>(value), -32768, 32767));
}

std::uint8_t trigger_value(std::int16_t value) noexcept {
  return static_cast<std::uint8_t>(
      std::clamp<int>(value, 0, 32767) >> 7);
}

PowerLevel power_level_from_percent(int percent) noexcept {
  if (percent < 0 || percent > 100) return PowerLevel::Unknown;
  if (percent <= 5) return PowerLevel::Empty;
  if (percent <= 25) return PowerLevel::Low;
  if (percent <= 70) return PowerLevel::Medium;
  return PowerLevel::Full;
}

class Sdl3Host final : public SdlHost {
 public:
  ~Sdl3Host() override { shutdown(); }

  Result setup() override {
    if (setup_) return Result::Success;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return Result::Failed;
    setup_ = true;
    return Result::Success;
  }

  void shutdown() noexcept override {
    {
      std::scoped_lock lock(mutex_);
      for (const auto& [id, gamepad] : gamepads_) {
        static_cast<void>(id);
        SDL_CloseGamepad(gamepad);
      }
      gamepads_.clear();
    }
    if (setup_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    setup_ = false;
  }

  int load_mappings_file(std::string_view path) override {
    return SDL_AddGamepadMappingsFromFile(std::string(path).c_str());
  }

  Result enumerate(std::vector<SdlHostDevice>& out) override {
    out.clear();
    if (!setup_) return Result::Failed;

    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids && count > 0) return Result::Failed;

    std::scoped_lock lock(mutex_);
    out.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int index = 0; index < count; ++index) {
      const auto id = ids[index];
      auto it = gamepads_.find(id);
      if (it == gamepads_.end()) {
        auto* gamepad = SDL_OpenGamepad(id);
        if (!gamepad) continue;
        it = gamepads_.emplace(id, gamepad).first;
      }
      out.push_back(describe(id, it->second));
    }

    for (auto it = gamepads_.begin(); it != gamepads_.end();) {
      const bool connected =
          std::find(ids, ids + count, it->first) != ids + count;
      if (connected) {
        ++it;
      } else {
        SDL_CloseGamepad(it->second);
        it = gamepads_.erase(it);
      }
    }
    SDL_free(ids);
    return Result::Success;
  }

  Result get_state(NativeDeviceId id, GamepadState& out) override {
    out = {};
    std::scoped_lock lock(mutex_);
    auto* gamepad = find(id);
    if (!gamepad) return Result::DeviceNotConnected;

    static constexpr std::array<std::pair<SDL_GamepadButton, std::uint16_t>, 14>
        kButtons{{
            {SDL_GAMEPAD_BUTTON_DPAD_UP, DpadUp},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, DpadDown},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, DpadLeft},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, DpadRight},
            {SDL_GAMEPAD_BUTTON_START, Start},
            {SDL_GAMEPAD_BUTTON_BACK, Back},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, LeftThumb},
            {SDL_GAMEPAD_BUTTON_RIGHT_STICK, RightThumb},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, LeftShoulder},
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, RightShoulder},
            {SDL_GAMEPAD_BUTTON_SOUTH, A},
            {SDL_GAMEPAD_BUTTON_EAST, B},
            {SDL_GAMEPAD_BUTTON_WEST, X},
            {SDL_GAMEPAD_BUTTON_NORTH, Y},
        }};
    for (const auto& [button, mask] : kButtons) {
      if (SDL_GetGamepadButton(gamepad, button)) out.buttons |= mask;
    }

    out.thumb_lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    out.thumb_ly =
        invert_axis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
    out.thumb_rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
    out.thumb_ry =
        invert_axis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
    out.left_trigger = trigger_value(
        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    out.right_trigger = trigger_value(
        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    return Result::Success;
  }

  Result rumble(NativeDeviceId id, const Vibration& vibration,
                std::uint32_t duration_ms) override {
    std::scoped_lock lock(mutex_);
    auto* gamepad = find(id);
    if (!gamepad) return Result::DeviceNotConnected;
    return SDL_RumbleGamepad(gamepad, vibration.left_motor_speed,
                             vibration.right_motor_speed, duration_ms)
               ? Result::Success
               : Result::Failed;
  }

  Result get_power_info(NativeDeviceId id, PowerInfo& out) override {
    out = {};
    std::scoped_lock lock(mutex_);
    auto* gamepad = find(id);
    if (!gamepad) return Result::DeviceNotConnected;

    int percent = -1;
    const auto state =
        SDL_GetJoystickPowerInfo(SDL_GetGamepadJoystick(gamepad), &percent);
    if (state == SDL_POWERSTATE_ERROR || state == SDL_POWERSTATE_UNKNOWN) {
      return Result::Unsupported;
    }

    out.source = state == SDL_POWERSTATE_NO_BATTERY ? PowerSource::Wired
                                                     : PowerSource::Battery;
    if (percent >= 0 && percent <= 100) {
      out.percentage = static_cast<std::uint8_t>(percent);
    }
    out.level = power_level_from_percent(percent);
    out.charging = state == SDL_POWERSTATE_CHARGING;
    return Result::Success;
  }

  Result set_player_index(NativeDeviceId id, std::uint8_t index) override {
    std::scoped_lock lock(mutex_);
    auto* gamepad = find(id);
    if (!gamepad) return Result::DeviceNotConnected;
    const int player =
        index == kNoPlayerIndicator ? -1 : static_cast<int>(index);
    return SDL_SetGamepadPlayerIndex(gamepad, player) ? Result::Success
                                                       : Result::Failed;
  }

  std::uint64_t now_millis() const noexcept override { return SDL_GetTicks(); }

 private:
  SdlHostDevice describe(SDL_JoystickID id, SDL_Gamepad* gamepad) const {
    SdlHostDevice device{};
    device.native_id = static_cast<NativeDeviceId>(id);
    if (const char* name = SDL_GetGamepadName(gamepad)) device.name = name;
    if (device.name.empty()) device.name = "SDL3 Gamepad";
    device.subtype = gamepad_subtype(SDL_GetGamepadType(gamepad));
    device.vendor_id = SDL_GetGamepadVendor(gamepad);
    device.product_id = SDL_GetGamepadProduct(gamepad);
    device.product_version = SDL_GetGamepadProductVersion(gamepad);
    if (const char* serial = SDL_GetGamepadSerial(gamepad)) device.serial = serial;
    if (const char* path = SDL_GetGamepadPath(gamepad)) device.path = path;

    char guid[33]{};
    SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(gamepad)), guid,
                     sizeof(guid));
    std::ostringstream key;
    key << guid << ':' << std::hex << std::setw(4) << std::setfill('0')
        << device.vendor_id << ':' << std::setw(4) << device.product_id
        << ":v=" << std::setw(4) << device.product_version;
    if (!device.serial.empty()) {
      key << ":serial=" << device.serial;
    } else if (!device.path.empty()) {
      key << ":path=" << device.path;
    } else {
      key << ":id=" << id;
    }
    device.persistent_key = key.str();

    int percent = -1;
    const auto power =
        SDL_GetJoystickPowerInfo(SDL_GetGamepadJoystick(gamepad), &percent);
    if (power == SDL_POWERSTATE_NO_BATTERY) {
      device.connection = ConnectionType::Wired;
    } else if (power == SDL_POWERSTATE_CHARGING ||
               power == SDL_POWERSTATE_CHARGED ||
               power == SDL_POWERSTATE_ON_BATTERY) {
      device.connection = ConnectionType::Wireless;
    }
    device.supports_vibration = true;
    device.supports_power_info = true;
    device.supports_player_indicator = true;
    return device;
  }

  SDL_Gamepad* find(NativeDeviceId id) const noexcept {
    const auto it = gamepads_.find(static_cast<SDL_JoystickID>(id));
    return it == gamepads_.end() ? nullptr : it->second;
  }

  bool setup_{};
  mutable std::mutex mutex_{};
  std::unordered_map<SDL_JoystickID, SDL_Gamepad*> gamepads_{};
};

}  // namespace

std::unique_ptr<SdlHost> create_sdl3_host() {
  return std::make_unique<Sdl3Host>();
}

}  // namespace xenon::input
