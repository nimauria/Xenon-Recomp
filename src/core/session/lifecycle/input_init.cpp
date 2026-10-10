#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>

#include "core/session/session_internal.hpp"
#include "xenon/input/null_driver.hpp"
#include "xenon/input/sdl_driver.hpp"
#if defined(_WIN32)
#include "xenon/input/xinput_driver.hpp"
#endif

namespace xenon::core {

using detail::ascii_lower;

bool XenonSession::init_input() {
  input_ = std::make_unique<input::InputSystem>();

  std::vector<std::string> requested = config_.input_drivers;
  if (requested.empty()) requested.push_back("automatic");

  bool added_real_driver = false;
  bool explicit_null = false;

  for (const auto& name : requested) {
    const std::string driver = ascii_lower(name);
    if (driver == "null" || driver == "none") {
      explicit_null = true;
      continue;
    }
    if (driver == "automatic" || driver == "auto") {
#if defined(_WIN32)
      if (auto xinput = input::create_xinput_driver()) {
        added_real_driver |= input_->add_driver(std::move(xinput));
      }
#endif
      if (auto sdl = input::create_sdl_input_driver()) {
        added_real_driver |= input_->add_driver(std::move(sdl));
      }
      continue;
    }
    if (driver == "xinput") {
#if defined(_WIN32)
      auto xinput = input::create_xinput_driver();
      if (!xinput) {
        set_error("XInput input driver requested but unavailable on this build");
        return false;
      }
      added_real_driver |= input_->add_driver(std::move(xinput));
#else
      set_error("XInput input driver requested but is only available on Windows");
      return false;
#endif
      continue;
    }
    if (driver == "sdl") {
      auto sdl = input::create_sdl_input_driver();
      if (!sdl) {
        set_error("SDL input driver requested but unavailable on this build");
        return false;
      }
      added_real_driver |= input_->add_driver(std::move(sdl));
      continue;
    }
    set_error("Unknown input driver requested: '" + name + "'");
    return false;
  }

  // Normal Play may not end up with a fully empty (zero real provider) input
  // system: that would silently strand every game that reads a controller.
  // Only an explicit "null"/"none" entry in config_.input_drivers is allowed
  // to produce a driver-less (or Null-driver-only) session, for headless/
  // test/developer configurations.
  if (!added_real_driver) {
    if (!explicit_null) {
      set_error(
          "Input subsystem requested but no real input provider (SDL/XInput) "
          "could be created on this build/platform");
      return false;
    }
    if (!input_->add_driver(std::make_unique<input::NullInputDriver>())) {
      set_error("Failed to install the explicit null input driver");
      return false;
    }
  }

  auto result = input_->setup();
  if (result != input::Result::Success) {
    set_error("Input system setup failed");
    return false;
  }

  // Apply the launcher's focus policy after setup so a foreground-only
  // session never leaks input while its presentation window is unfocused.
  input_->set_background_input_policy(
      config_.input_background ? input::BackgroundInputPolicy::Always
                               : input::BackgroundInputPolicy::ForegroundOnly);

  // The launcher exposes one simple global deadzone.  Feed it into the
  // default runtime profile rather than duplicating deadzone math in the
  // session/runtime host.  Per-device/user profile bindings still override
  // this default through ProfileStore as usual.
  if (auto profile = input_->profiles().profile("default")) {
    const auto dz = static_cast<float>(std::clamp(config_.input_deadzone, 0.0, 0.95));
    profile->left_stick.inner_deadzone = dz;
    profile->right_stick.inner_deadzone = dz;
    if (!input_->profiles().upsert(std::move(*profile))) {
      set_error("Failed to apply default input deadzone profile");
      return false;
    }
  }

  // Load persistent input profiles when the launcher supplied its profile
  // store.  A missing file is allowed on first run; malformed existing files
  // remain a hard error because silently discarding user mappings is worse
  // than surfacing the configuration problem.
  if (!config_.input_profile_store_path.empty()) {
    const std::filesystem::path profile_path(config_.input_profile_store_path);
    std::error_code ec;
    if (std::filesystem::exists(profile_path, ec) && !ec &&
        !input_->profiles().load(profile_path)) {
      set_error("Failed to load input profile store: '" +
                config_.input_profile_store_path + "'");
      return false;
    }
  }

  const auto match_device = [this](std::string_view selector)
      -> std::optional<input::DeviceId> {
    if (selector.empty()) return std::nullopt;
    const auto wanted = ascii_lower(selector);
    if (wanted == "automatic" || wanted == "auto") return std::nullopt;
    for (const auto& device : input_->devices()) {
      if (!device.connected) continue;
      if (ascii_lower(device.identity_key) == wanted ||
          ascii_lower(device.persistent_key) == wanted ||
          ascii_lower(device.name) == wanted ||
          ascii_lower(device.driver_name) == wanted) {
        return device.id;
      }
    }
    return std::nullopt;
  };

  // Preferred device is an explicit user-0 override.  If the selector no
  // longer exists (device unplugged/renamed), retain InputSystem's automatic
  // assignment rather than failing the entire game launch.
  if (auto preferred = match_device(config_.input_preferred_device)) {
    static_cast<void>(input_->assign_user(0, *preferred));
  }

  // Add launcher-defined multi-source routes (controller + keyboard/HOTAS,
  // accessibility devices, etc.). Unknown selectors are intentionally
  // ignored here so hotplug can be reconciled by a later frontend refresh.
  // SessionConfig stores one selector list per Xbox user; runtime_host fills
  // the same fixed table by launch-config userIndex.
  for (std::uint32_t user_index = 0; user_index < input::kMaxUsers; ++user_index) {
    const auto& sources = config_.input_user_sources[user_index];
    bool primary_selected = input_->device_for_user(user_index).has_value();
    for (const auto& selector : sources) {
      const auto device = match_device(selector);
      if (!device) continue;
      if (!primary_selected) {
        if (input_->assign_user(user_index, *device) == input::Result::Success) {
          primary_selected = true;
        }
      } else {
        static_cast<void>(input_->add_user_source(user_index, *device));
      }
    }
  }

  // input_rumble is retained in SessionConfig for the launcher/runtime
  // contract.  The current InputSystem has no global force-feedback gate;
  // GuestInputBridge continues to use the per-device capability path.  This
  // avoids lying by pretending a session-level toggle is already enforced.
  // A dedicated InputSystem vibration policy can consume this field later.

  input_bridge_ = std::make_unique<input::xam::guest::GuestInputBridge>(*input_);
  return true;
}

}  // namespace xenon::core
