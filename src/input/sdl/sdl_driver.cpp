#include "xenon/input/sdl_driver.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <utility>

namespace xenon::input {
namespace {

constexpr std::uint16_t kVkNone = 0;
constexpr std::array<std::uint16_t, 34> kVirtualKeys = {
    0x5810,  // D-pad up
    0x5811,  // D-pad down
    0x5812,  // D-pad left
    0x5813,  // D-pad right
    0x5814,  // Start
    0x5815,  // Back
    0x5816,  // Left thumb press
    0x5817,  // Right thumb press
    0x5805,  // Left shoulder
    0x5804,  // Right shoulder
    kVkNone, // Guide
    kVkNone, // unused button bit
    0x5800,  // A
    0x5801,  // B
    0x5802,  // X
    0x5803,  // Y
    0x5806,  // Left trigger
    0x5807,  // Right trigger
    0x5820,  // Left thumb up
    0x5821,  // Left thumb down
    0x5822,  // Left thumb right
    0x5823,  // Left thumb left
    0x5824,  // Left thumb up-left
    0x5825,  // Left thumb up-right
    0x5826,  // Left thumb down-right
    0x5827,  // Left thumb down-left
    0x5830,  // Right thumb up
    0x5831,  // Right thumb down
    0x5832,  // Right thumb right
    0x5833,  // Right thumb left
    0x5834,  // Right thumb up-left
    0x5835,  // Right thumb up-right
    0x5836,  // Right thumb down-right
    0x5837,  // Right thumb down-left
};

}  // namespace

SdlInputDriver::SdlInputDriver(SdlInputOptions options)
    : SdlInputDriver(create_preferred_sdl_host(), std::move(options)) {}

SdlInputDriver::SdlInputDriver(std::unique_ptr<SdlHost> host,
                               SdlInputOptions options)
    : host_(std::move(host)), options_(std::move(options)) {
  if (options_.repeat_rate_ms == 0) options_.repeat_rate_ms = 1;
  if (options_.repeat_delay_ms < options_.repeat_rate_ms) {
    options_.repeat_delay_ms = options_.repeat_rate_ms;
  }
  if (options_.thumb_keystroke_threshold < 0) {
    options_.thumb_keystroke_threshold =
        static_cast<std::int16_t>(-options_.thumb_keystroke_threshold);
  }
}

SdlInputDriver::~SdlInputDriver() { shutdown(); }

Result SdlInputDriver::setup() {
  if (setup_) return Result::Success;
  if (!host_) return Result::Unsupported;
  const auto result = host_->setup();
  if (result != Result::Success) return result;
  setup_ = true;

  loaded_mapping_count_ = 0;
  if (options_.load_mappings_if_present && !options_.mappings_file.empty()) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(options_.mappings_file, ec)) {
      const auto mappings = host_->load_mappings_file(options_.mappings_file);
      if (mappings >= 0) loaded_mapping_count_ = mappings;
    }
  }
  return refresh_cache();
}

void SdlInputDriver::shutdown() noexcept {
  if (!setup_) return;
  if (host_) host_->shutdown();
  devices_.clear();
  keystrokes_.clear();
  setup_ = false;
  loaded_mapping_count_ = 0;
}

Result SdlInputDriver::refresh_cache() {
  if (!setup_ || !host_) return Result::Failed;
  std::vector<SdlHostDevice> refreshed;
  const auto result = host_->enumerate(refreshed);
  if (result != Result::Success) return result;
  std::sort(refreshed.begin(), refreshed.end(), [](const auto& lhs,
                                                   const auto& rhs) {
    if (lhs.persistent_key != rhs.persistent_key) {
      return lhs.persistent_key < rhs.persistent_key;
    }
    return lhs.native_id < rhs.native_id;
  });
  devices_ = std::move(refreshed);

  keystrokes_.erase(
      std::remove_if(keystrokes_.begin(), keystrokes_.end(),
                     [&](const auto& item) {
                       return std::none_of(
                           devices_.begin(), devices_.end(),
                           [&](const auto& device) {
                             return device.native_id == item.first;
                           });
                     }),
      keystrokes_.end());
  return Result::Success;
}

void SdlInputDriver::enumerate_devices(
    std::vector<DriverDeviceInfo>& out_devices) {
  out_devices.clear();
  if (!setup_ || refresh_cache() != Result::Success) return;
  out_devices.reserve(devices_.size());
  for (const auto& device : devices_) {
    DriverDeviceInfo info{};
    info.native_id = device.native_id;
    info.persistent_key = device.persistent_key;
    info.name = device.name;
    info.type = DeviceType::Gamepad;
    info.subtype = device.subtype;
    info.connection = device.connection;
    info.vendor_id = device.vendor_id;
    info.product_id = device.product_id;
    info.product_version = device.product_version;
    info.serial = device.serial;
    info.path = device.path;
    info.supports_vibration = device.supports_vibration;
    info.supports_keystrokes = true;
    info.supports_power_info = device.supports_power_info;
    info.supports_player_indicator = device.supports_player_indicator;
    out_devices.push_back(std::move(info));
  }
}

Result SdlInputDriver::get_state(NativeDeviceId device,
                                 GamepadState& out_state) {
  out_state = {};
  if (!setup_ || !host_) return Result::Failed;
  return host_->get_state(device, out_state);
}

Result SdlInputDriver::get_capabilities(NativeDeviceId device,
                                        Capabilities& out_caps) {
  out_caps = {};
  if (!setup_ || !host_) return Result::Failed;
  auto find_device = [&]() {
    return std::find_if(devices_.begin(), devices_.end(),
                        [&](const auto& item) {
                          return item.native_id == device;
                        });
  };
  auto it = find_device();
  if (it == devices_.end()) {
    if (refresh_cache() != Result::Success) return Result::Failed;
    it = find_device();
    if (it == devices_.end()) return Result::DeviceNotConnected;
  }

  out_caps.type = DeviceType::Gamepad;
  out_caps.subtype = it->subtype;
  out_caps.flags = CapabilityKeystrokeSupported;
  if (it->supports_vibration) {
    out_caps.flags |= CapabilityVibrationSupported;
    out_caps.vibration = {0xFFFFu, 0xFFFFu};
  }
  out_caps.gamepad.buttons = 0xF7FFu;
  out_caps.gamepad.left_trigger = 0xFFu;
  out_caps.gamepad.right_trigger = 0xFFu;
  out_caps.gamepad.thumb_lx = static_cast<std::int16_t>(0xFFFFu);
  out_caps.gamepad.thumb_ly = static_cast<std::int16_t>(0xFFFFu);
  out_caps.gamepad.thumb_rx = static_cast<std::int16_t>(0xFFFFu);
  out_caps.gamepad.thumb_ry = static_cast<std::int16_t>(0xFFFFu);
  return Result::Success;
}

Result SdlInputDriver::set_vibration(NativeDeviceId device,
                                     const Vibration& vibration) {
  if (!setup_ || !host_) return Result::Failed;
  const auto it = std::find_if(devices_.begin(), devices_.end(),
                               [&](const auto& item) {
                                 return item.native_id == device;
                               });
  if (it == devices_.end()) return Result::DeviceNotConnected;
  if (!it->supports_vibration) {
    return vibration == Vibration{} ? Result::Success : Result::Unsupported;
  }
  return host_->rumble(device, vibration, options_.rumble_duration_ms);
}

Result SdlInputDriver::get_power_info(NativeDeviceId device,
                                          PowerInfo& out_power) {
  out_power = {};
  if (!setup_ || !host_) return Result::Failed;
  const auto it = std::find_if(devices_.begin(), devices_.end(),
                               [&](const auto& item) {
                                 return item.native_id == device;
                               });
  if (it == devices_.end()) return Result::DeviceNotConnected;
  if (!it->supports_power_info) return Result::Unsupported;
  return host_->get_power_info(device, out_power);
}

Result SdlInputDriver::set_player_indicator(NativeDeviceId device,
                                            std::uint8_t player_index) {
  if (!setup_ || !host_) return Result::Failed;
  const auto it = std::find_if(devices_.begin(), devices_.end(),
                               [&](const auto& item) {
                                 return item.native_id == device;
                               });
  if (it == devices_.end()) return Result::DeviceNotConnected;
  if (!it->supports_player_indicator) return Result::Unsupported;
  return host_->set_player_index(device, player_index);
}

std::uint64_t SdlInputDriver::analog_to_keyfield(
    const GamepadState& state) const noexcept {
  std::uint64_t result = 0;
  result |= static_cast<std::uint64_t>(
                state.left_trigger > options_.trigger_keystroke_threshold)
            << 16u;
  result |= static_cast<std::uint64_t>(
                state.right_trigger > options_.trigger_keystroke_threshold)
            << 17u;

  auto add_stick = [&](std::int16_t x, std::int16_t y,
                       std::uint32_t base) {
    const auto threshold = options_.thumb_keystroke_threshold;
    bool up = y > threshold;
    bool down = y < -threshold;
    bool right = x > threshold;
    bool left = x < -threshold;
    if (up && left) {
      up = left = false;
      result |= std::uint64_t{1} << (base + 4u);
    }
    if (up && right) {
      up = right = false;
      result |= std::uint64_t{1} << (base + 5u);
    }
    if (down && right) {
      down = right = false;
      result |= std::uint64_t{1} << (base + 6u);
    }
    if (down && left) {
      down = left = false;
      result |= std::uint64_t{1} << (base + 7u);
    }
    if (up) result |= std::uint64_t{1} << base;
    if (down) result |= std::uint64_t{1} << (base + 1u);
    if (right) result |= std::uint64_t{1} << (base + 2u);
    if (left) result |= std::uint64_t{1} << (base + 3u);
  };
  add_stick(state.thumb_lx, state.thumb_ly, 18u);
  add_stick(state.thumb_rx, state.thumb_ry, 26u);
  return result;
}

Result SdlInputDriver::get_keystroke(NativeDeviceId device,
                                     Keystroke& out_keystroke) {
  out_keystroke = {};
  GamepadState state{};
  const auto state_result = get_state(device, state);
  if (state_result != Result::Success) return state_result;

  auto tracker = std::find_if(keystrokes_.begin(), keystrokes_.end(),
                              [&](const auto& item) {
                                return item.first == device;
                              });
  if (tracker == keystrokes_.end()) {
    keystrokes_.push_back({device, {}});
    tracker = std::prev(keystrokes_.end());
  }
  auto& last = tracker->second;
  const auto current = static_cast<std::uint64_t>(state.buttons) |
                       analog_to_keyfield(state);
  const auto now = host_->now_millis();

  if (last.repeat_state == RepeatState::Waiting &&
      now >= last.repeat_time + options_.repeat_delay_ms) {
    last.repeat_state = RepeatState::Repeating;
  }
  if (last.repeat_state == RepeatState::Repeating &&
      now >= last.repeat_time + options_.repeat_rate_ms) {
    const auto key = kVirtualKeys.at(last.repeat_button);
    if (key != kVkNone &&
        (current & (std::uint64_t{1} << last.repeat_button)) != 0) {
      last.repeat_time = now;
      out_keystroke.virtual_key = key;
      out_keystroke.flags = KeystrokeKeyDown | KeystrokeRepeat;
      return Result::Success;
    }
    last.repeat_state = RepeatState::Idle;
  }

  const auto changed = current ^ last.buttons;
  if (!changed) return Result::Empty;

  // XInput reports releases before presses when a single poll changes several
  // logical directions (for example up-left -> left).
  for (int pass = 0; pass < 2; ++pass) {
    const bool release_pass = pass == 0;
    for (std::uint8_t bit = 0; bit < kVirtualKeys.size(); ++bit) {
      const auto mask = std::uint64_t{1} << bit;
      if ((changed & mask) == 0) continue;
      const auto key = kVirtualKeys[bit];
      if (key == kVkNone) {
        last.buttons = (last.buttons & ~mask) | (current & mask);
        continue;
      }
      const bool pressed = (current & mask) != 0;
      if (release_pass != !pressed) continue;

      out_keystroke.virtual_key = key;
      if (pressed) {
        out_keystroke.flags = KeystrokeKeyDown;
        last.buttons |= mask;
        last.repeat_state = RepeatState::Waiting;
        last.repeat_button = bit;
        last.repeat_time = now;
      } else {
        out_keystroke.flags = KeystrokeKeyUp;
        last.buttons &= ~mask;
        if (last.repeat_button == bit) last.repeat_state = RepeatState::Idle;
      }
      return Result::Success;
    }
  }
  return Result::Empty;
}

std::unique_ptr<InputDriver> create_sdl_input_driver(SdlInputOptions options) {
  return std::make_unique<SdlInputDriver>(std::move(options));
}

#if !defined(XENON_INPUT_HAS_SDL2)
std::unique_ptr<SdlHost> create_sdl2_host() { return {}; }
#endif
#if !defined(XENON_INPUT_HAS_SDL3)
std::unique_ptr<SdlHost> create_sdl3_host() { return {}; }
#endif
std::unique_ptr<SdlHost> create_preferred_sdl_host() {
  if (auto host = create_sdl3_host()) return host;
  return create_sdl2_host();
}

}  // namespace xenon::input
