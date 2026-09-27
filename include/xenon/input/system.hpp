#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenon/input/driver.hpp"
#include "xenon/input/action_router.hpp"
#include "xenon/input/profile.hpp"

namespace xenon::input {

class InputSystem {
 public:
  InputSystem() = default;
  ~InputSystem();

  InputSystem(const InputSystem&) = delete;
  InputSystem& operator=(const InputSystem&) = delete;

  // Drivers must be registered before setup(). Driver ownership transfers to
  // the input system.
  [[nodiscard]] bool add_driver(std::unique_ptr<InputDriver> driver);
  [[nodiscard]] Result setup();
  void shutdown() noexcept;

  // Reconciles hotplug changes. Stable IDs and ordinals are retained for known
  // persistent device keys and are never recycled during the process lifetime.
  void refresh_devices();

  [[nodiscard]] std::vector<DeviceInfo> devices() const;
  [[nodiscard]] std::optional<DeviceInfo> device(DeviceId id) const;
  [[nodiscard]] std::optional<DeviceId> device_for_user(
      std::uint32_t user_index) const;

  [[nodiscard]] Result assign_user(std::uint32_t user_index, DeviceId device_id);
  [[nodiscard]] Result clear_user(std::uint32_t user_index);
  // Replaces a user's complete route atomically. Explicit routes are sticky:
  // their stable identities remain configured while devices are disconnected
  // and are restored automatically on reconnect. An empty route intentionally
  // leaves the user unassigned.
  [[nodiscard]] Result set_user_sources(std::uint32_t user_index,
                                        std::span<const DeviceId> device_ids);
  [[nodiscard]] Result set_user_source_identities(
      std::uint32_t user_index, std::vector<std::string> identity_keys);
  [[nodiscard]] std::vector<std::string> desired_sources_for_user(
      std::uint32_t user_index) const;
  void set_auto_assignment(std::uint32_t user_index, bool enabled);
  [[nodiscard]] bool auto_assignment(std::uint32_t user_index) const;
  // Additional sources are merged into the primary device for this guest user.
  // This enables controller + keyboard/mouse + HOTAS/accessibility devices.
  [[nodiscard]] Result add_user_source(std::uint32_t user_index, DeviceId device_id);
  [[nodiscard]] Result remove_user_source(std::uint32_t user_index, DeviceId device_id);
  [[nodiscard]] std::vector<DeviceId> sources_for_user(std::uint32_t user_index) const;

  // When inactive, state polling returns a neutral connected state and
  // vibration requests are suppressed. This lets a launcher/overlay gate input
  // without teaching individual host drivers about frontend focus policy.
  void set_active(bool active);
  [[nodiscard]] bool active() const noexcept;
  void set_focused(bool focused);
  [[nodiscard]] bool focused() const noexcept;
  void set_background_input_policy(BackgroundInputPolicy policy);
  [[nodiscard]] BackgroundInputPolicy background_input_policy() const noexcept;
  void set_vibration_enabled(bool enabled);
  [[nodiscard]] bool vibration_enabled() const noexcept;
  [[nodiscard]] bool effective_active() const noexcept;

  [[nodiscard]] Result get_state(std::uint32_t user_index, State& out_state);
  [[nodiscard]] Result get_capabilities(std::uint32_t user_index,
                                        std::uint32_t flags,
                                        Capabilities& out_caps);
  [[nodiscard]] Result set_vibration(std::uint32_t user_index,
                                     const Vibration& vibration);
  [[nodiscard]] Result get_keystroke(std::uint32_t user_index,
                                     std::uint32_t flags,
                                     Keystroke& out_keystroke);
  [[nodiscard]] Result get_power_info(std::uint32_t user_index,
                                     PowerInfo& out_power);
  [[nodiscard]] Result set_player_indicator(std::uint32_t user_index,
                                            std::uint8_t player_index);
  [[nodiscard]] Result get_motion_state(std::uint32_t user_index,
                                        MotionState& out_motion);
  [[nodiscard]] Result get_touchpad_state(std::uint32_t user_index,
                                          TouchpadState& out_touch);
  [[nodiscard]] Result set_light_color(std::uint32_t user_index,
                                       const LightColor& color);
  [[nodiscard]] InputDiagnostics diagnostics() const;

  // Shared frontend action routing lives beside the device core so all host
  // input forms can feed the same action queue before a UI consumes it.
  [[nodiscard]] FrontendInputRouter& frontend_router() noexcept {
    return frontend_router_;
  }
  [[nodiscard]] const FrontendInputRouter& frontend_router() const noexcept {
    return frontend_router_;
  }

  [[nodiscard]] ProfileStore& profiles() noexcept { return profiles_; }
  [[nodiscard]] const ProfileStore& profiles() const noexcept { return profiles_; }

 private:
  struct DriverSlot {
    std::unique_ptr<InputDriver> driver{};
    std::string name{};
    bool setup{};
  };

  struct DeviceRecord {
    DeviceInfo info{};
    std::size_t driver_index{};
    NativeDeviceId native_id{};
    Vibration last_vibration{};
    bool has_state{};
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
    std::uint32_t refresh_generation{};
  };

  struct RoutedDevice {
    DeviceId id{kInvalidDeviceId};
    std::size_t driver_index{};
    NativeDeviceId native_id{};
    std::string identity_key{};
  };

  [[nodiscard]] std::optional<RoutedDevice> route_user_locked(
      std::uint32_t user_index) const;
  [[nodiscard]] Result poll_state_source(std::uint32_t user_index,
                                         const RoutedDevice& route,
                                         InputDriver& driver,
                                         GamepadState& out_state);
  void write_merged_state_locked(std::uint32_t user_index,
                                 const GamepadState& merged,
                                 State& out_state);
  void auto_assign_locked();
  void reconcile_user_routes_locked();
  [[nodiscard]] std::optional<DeviceId> connected_id_for_identity_locked(
      std::string_view identity) const;
  [[nodiscard]] static std::string make_identity_key(
      std::string_view driver_name, std::string_view persistent_key);
  [[nodiscard]] bool effective_active_locked() const noexcept;
  void stop_vibration_for_inactive_transition(bool was_active, bool now_active);
  void sync_player_indicators();

  // Serializes driver lifecycle, hotplug enumeration, and I/O. The routing
  // mutex below protects Xenon's records; host drivers are not required to
  // tolerate refresh and polling concurrently.
  mutable std::mutex driver_mutex_{};
  mutable std::mutex mutex_{};
  std::vector<DriverSlot> drivers_{};
  std::unordered_map<DeviceId, DeviceRecord> records_{};
  std::unordered_map<std::string, DeviceId> identity_to_id_{};
  std::array<DeviceId, kMaxUsers> users_{};
  std::array<std::vector<DeviceId>, kMaxUsers> extra_sources_{};
  std::array<std::vector<std::string>, kMaxUsers>
      desired_source_identities_{};
  std::array<bool, kMaxUsers> auto_assignment_{{true, true, true, true}};
  std::array<GamepadState, kMaxUsers> merged_last_state_{};
  std::array<std::uint32_t, kMaxUsers> merged_packet_number_{};
  std::array<bool, kMaxUsers> merged_has_state_{};
  DeviceId next_device_id_{1};
  std::uint32_t next_ordinal_{};
  std::uint32_t refresh_generation_{1};
  bool setup_{};
  bool active_{true};
  bool focused_{true};
  BackgroundInputPolicy background_policy_{BackgroundInputPolicy::ForegroundOnly};
  bool vibration_enabled_{true};
  ProfileStore profiles_{};
  FrontendInputRouter frontend_router_{};
};

}  // namespace xenon::input
