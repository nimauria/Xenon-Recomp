#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenon/input/driver.hpp"
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
  [[nodiscard]] InputDiagnostics diagnostics() const;

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
    std::string_view identity_key{};
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
  [[nodiscard]] static std::string make_identity_key(
      std::string_view driver_name, std::string_view persistent_key);
  [[nodiscard]] bool effective_active_locked() const noexcept;
  void stop_vibration_for_inactive_transition(bool was_active, bool now_active);
  void sync_player_indicators();

  mutable std::mutex mutex_{};
  std::vector<DriverSlot> drivers_{};
  std::unordered_map<DeviceId, DeviceRecord> records_{};
  std::unordered_map<std::string, DeviceId> identity_to_id_{};
  std::array<DeviceId, kMaxUsers> users_{};
  std::array<std::vector<DeviceId>, kMaxUsers> extra_sources_{};
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
  ProfileStore profiles_{};
};

}  // namespace xenon::input
