#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

#include <array>
#include <memory>
#include <string>

#include "xenon/input/xinput_driver.hpp"

namespace xenon::input {
namespace {

Result map_result(DWORD value) noexcept {
  switch (value) {
    case ERROR_SUCCESS:
      return Result::Success;
    case ERROR_DEVICE_NOT_CONNECTED:
      return Result::DeviceNotConnected;
    case ERROR_EMPTY:
      return Result::Empty;
    case ERROR_BAD_ARGUMENTS:
    case ERROR_INVALID_PARAMETER:
      return Result::BadArguments;
    default:
      return Result::Failed;
  }
}

DeviceSubtype map_subtype(BYTE value) noexcept {
  switch (value) {
    case XINPUT_DEVSUBTYPE_WHEEL:
      return DeviceSubtype::Wheel;
    case XINPUT_DEVSUBTYPE_ARCADE_STICK:
      return DeviceSubtype::ArcadeStick;
    case XINPUT_DEVSUBTYPE_FLIGHT_STICK:
      return DeviceSubtype::FlightStick;
    case XINPUT_DEVSUBTYPE_DANCE_PAD:
      return DeviceSubtype::DancePad;
    case XINPUT_DEVSUBTYPE_GUITAR:
      return DeviceSubtype::Guitar;
    case XINPUT_DEVSUBTYPE_DRUM_KIT:
      return DeviceSubtype::DrumKit;
    default:
      return DeviceSubtype::Gamepad;
  }
}

bool valid_device(NativeDeviceId id) noexcept { return id >= 1 && id <= 4; }
DWORD slot_from_device(NativeDeviceId id) noexcept {
  return static_cast<DWORD>(id - 1);
}

class Win32XInputHost final : public XInputHost {
 public:
  ~Win32XInputHost() override { shutdown(); }

  Result setup() override {
    if (module_) return Result::Success;

    static constexpr std::array<const wchar_t*, 3> kLibraries{
        L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const auto* library : kLibraries) {
      module_ = LoadLibraryW(library);
      if (module_) break;
    }
    if (!module_) return Result::Unsupported;

    get_caps_ = load<GetCapsFn>("XInputGetCapabilities");
    get_state_ = load<GetStateFn>("XInputGetState");
    get_state_ex_ = reinterpret_cast<GetStateFn>(
        GetProcAddress(module_, reinterpret_cast<LPCSTR>(100)));
    get_key_ = load<GetKeyFn>("XInputGetKeystroke");
    set_state_ = load<SetStateFn>("XInputSetState");
    get_battery_ = load<GetBatteryFn>("XInputGetBatteryInformation");

    if (!get_caps_ || !get_state_ || !set_state_) {
      shutdown();
      return Result::Unsupported;
    }
    return Result::Success;
  }

  void shutdown() noexcept override {
    if (module_) FreeLibrary(module_);
    module_ = nullptr;
    get_caps_ = nullptr;
    get_state_ = nullptr;
    get_state_ex_ = nullptr;
    get_key_ = nullptr;
    set_state_ = nullptr;
    get_battery_ = nullptr;
  }

  Result enumerate(std::vector<XInputHostDevice>& out) override {
    out.clear();
    out.reserve(4);
    for (DWORD slot = 0; slot < 4; ++slot) {
      XINPUT_CAPABILITIES caps{};
      if (get_caps_(slot, 0, &caps) != ERROR_SUCCESS) continue;

      XInputHostDevice device{};
      device.native_id = slot + 1;
      device.persistent_key = "slot:" + std::to_string(slot);
      device.name = "XInput Controller";
      device.subtype = map_subtype(caps.SubType);
      device.supports_vibration = true;
      device.supports_keystrokes = get_key_ != nullptr;
      device.supports_power_info = get_battery_ != nullptr;
      out.push_back(std::move(device));
    }
    return Result::Success;
  }

  Result get_state(NativeDeviceId id, GamepadState& out) override {
    out = {};
    if (!valid_device(id)) return Result::DeviceNotConnected;

    // The undocumented state-ex entry may write one DWORD beyond XINPUT_STATE.
    struct ExtendedState {
      XINPUT_STATE state{};
      DWORD reserved{};
    } native;
    const auto get_state = get_state_ex_ ? get_state_ex_ : get_state_;
    const DWORD result = get_state(slot_from_device(id), &native.state);
    if (result != ERROR_SUCCESS) return map_result(result);

    const auto& pad = native.state.Gamepad;
    out.buttons = pad.wButtons;
    out.left_trigger = pad.bLeftTrigger;
    out.right_trigger = pad.bRightTrigger;
    out.thumb_lx = pad.sThumbLX;
    out.thumb_ly = pad.sThumbLY;
    out.thumb_rx = pad.sThumbRX;
    out.thumb_ry = pad.sThumbRY;
    return Result::Success;
  }

  Result get_capabilities(NativeDeviceId id, Capabilities& out) override {
    out = {};
    if (!valid_device(id)) return Result::DeviceNotConnected;

    XINPUT_CAPABILITIES native{};
    const DWORD result = get_caps_(slot_from_device(id), 0, &native);
    if (result != ERROR_SUCCESS) return map_result(result);

    out.type = DeviceType::Gamepad;
    out.subtype = map_subtype(native.SubType);
    out.flags = CapabilityVibrationSupported;
    if (get_key_) out.flags |= CapabilityKeystrokeSupported;
    out.gamepad.buttons = native.Gamepad.wButtons;
    out.gamepad.left_trigger = native.Gamepad.bLeftTrigger;
    out.gamepad.right_trigger = native.Gamepad.bRightTrigger;
    out.gamepad.thumb_lx = native.Gamepad.sThumbLX;
    out.gamepad.thumb_ly = native.Gamepad.sThumbLY;
    out.gamepad.thumb_rx = native.Gamepad.sThumbRX;
    out.gamepad.thumb_ry = native.Gamepad.sThumbRY;
    out.vibration = {native.Vibration.wLeftMotorSpeed,
                     native.Vibration.wRightMotorSpeed};
    return Result::Success;
  }

  Result set_vibration(NativeDeviceId id, const Vibration& value) override {
    if (!valid_device(id)) return Result::DeviceNotConnected;
    XINPUT_VIBRATION native{value.left_motor_speed, value.right_motor_speed};
    return map_result(set_state_(slot_from_device(id), &native));
  }

  Result get_keystroke(NativeDeviceId id, Keystroke& out) override {
    out = {};
    if (!get_key_) return Result::Unsupported;
    if (!valid_device(id)) return Result::DeviceNotConnected;

    XINPUT_KEYSTROKE native{};
    const DWORD result = get_key_(slot_from_device(id), 0, &native);
    if (result != ERROR_SUCCESS) return map_result(result);

    out.virtual_key = native.VirtualKey;
    out.unicode = static_cast<char16_t>(native.Unicode);
    out.flags = native.Flags;
    out.user_index = native.UserIndex;
    out.hid_code = native.HidCode;
    return Result::Success;
  }

  Result get_power_info(NativeDeviceId id, PowerInfo& out) override {
    out = {};
    if (!get_battery_) return Result::Unsupported;
    if (!valid_device(id)) return Result::DeviceNotConnected;

    XINPUT_BATTERY_INFORMATION native{};
    const DWORD result = get_battery_(slot_from_device(id),
                                      BATTERY_DEVTYPE_GAMEPAD, &native);
    if (result != ERROR_SUCCESS) return map_result(result);

    out.source = native.BatteryType == BATTERY_TYPE_WIRED
                     ? PowerSource::Wired
                     : PowerSource::Battery;
    switch (native.BatteryLevel) {
      case BATTERY_LEVEL_EMPTY:
        out.level = PowerLevel::Empty;
        break;
      case BATTERY_LEVEL_LOW:
        out.level = PowerLevel::Low;
        break;
      case BATTERY_LEVEL_MEDIUM:
        out.level = PowerLevel::Medium;
        break;
      case BATTERY_LEVEL_FULL:
        out.level = PowerLevel::Full;
        break;
      default:
        out.level = PowerLevel::Unknown;
        break;
    }
    return Result::Success;
  }

 private:
  using GetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
  using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
  using GetKeyFn = DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE);
  using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
  using GetBatteryFn =
      DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);

  template <typename T>
  T load(const char* name) const noexcept {
    return reinterpret_cast<T>(GetProcAddress(module_, name));
  }

  HMODULE module_{};
  GetCapsFn get_caps_{};
  GetStateFn get_state_{};
  GetStateFn get_state_ex_{};
  GetKeyFn get_key_{};
  SetStateFn set_state_{};
  GetBatteryFn get_battery_{};
};

}  // namespace

std::unique_ptr<XInputHost> create_win32_xinput_host() {
  return std::make_unique<Win32XInputHost>();
}

}  // namespace xenon::input
#endif
