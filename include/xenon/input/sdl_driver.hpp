#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/input/driver.hpp"

namespace xenon::input {

// SDL itself is deliberately hidden behind this host adapter. The controller
// routing / Xbox mapping layer remains unit-testable without SDL installed.
// SDL2 and SDL3 are interchangeable host adapters behind this interface.
struct SdlHostDevice {
  NativeDeviceId native_id{};
  std::string persistent_key{};
  std::string name{};
  DeviceSubtype subtype{DeviceSubtype::Gamepad};
  ConnectionType connection{ConnectionType::Unknown};
  std::uint16_t vendor_id{};
  std::uint16_t product_id{};
  std::uint16_t product_version{};
  std::string serial{};
  std::string path{};
  bool supports_vibration{};
  bool supports_power_info{};
  bool supports_player_indicator{};
};

class SdlHost {
 public:
  virtual ~SdlHost() = default;

  [[nodiscard]] virtual Result setup() = 0;
  virtual void shutdown() noexcept = 0;
  [[nodiscard]] virtual int load_mappings_file(std::string_view path) = 0;
  [[nodiscard]] virtual Result enumerate(
      std::vector<SdlHostDevice>& out_devices) = 0;
  [[nodiscard]] virtual Result get_state(NativeDeviceId device,
                                         GamepadState& out_state) = 0;
  [[nodiscard]] virtual Result rumble(NativeDeviceId device,
                                      const Vibration& vibration,
                                      std::uint32_t duration_ms) = 0;
  [[nodiscard]] virtual Result get_power_info(NativeDeviceId device,
                                              PowerInfo& out_power) = 0;
  [[nodiscard]] virtual Result set_player_index(NativeDeviceId device,
                                                std::uint8_t player_index) = 0;
  [[nodiscard]] virtual std::uint64_t now_millis() const noexcept = 0;
};

struct SdlInputOptions {
  std::string mappings_file{"gamecontrollerdb.txt"};
  bool load_mappings_if_present{true};
  std::uint32_t rumble_duration_ms{0xFFFFu};
  std::uint32_t repeat_delay_ms{400};
  std::uint32_t repeat_rate_ms{100};
  std::uint8_t trigger_keystroke_threshold{0x1Fu};
  std::int16_t thumb_keystroke_threshold{0x4E00};
};

class SdlInputDriver final : public InputDriver {
 public:
  explicit SdlInputDriver(SdlInputOptions options = {});
  SdlInputDriver(std::unique_ptr<SdlHost> host,
                 SdlInputOptions options = {});
  ~SdlInputDriver() override;

  [[nodiscard]] std::string_view name() const noexcept override { return "sdl"; }
  [[nodiscard]] Result setup() override;
  void shutdown() noexcept override;
  void enumerate_devices(std::vector<DriverDeviceInfo>& out_devices) override;
  [[nodiscard]] Result get_state(NativeDeviceId device,
                                 GamepadState& out_state) override;
  [[nodiscard]] Result get_capabilities(NativeDeviceId device,
                                        Capabilities& out_caps) override;
  [[nodiscard]] Result set_vibration(NativeDeviceId device,
                                     const Vibration& vibration) override;
  [[nodiscard]] Result get_keystroke(NativeDeviceId device,
                                     Keystroke& out_keystroke) override;
  [[nodiscard]] Result get_power_info(NativeDeviceId device,
                                      PowerInfo& out_power) override;
  [[nodiscard]] Result set_player_indicator(
      NativeDeviceId device, std::uint8_t player_index) override;

  [[nodiscard]] bool available() const noexcept { return host_ != nullptr; }
  [[nodiscard]] int loaded_mapping_count() const noexcept {
    return loaded_mapping_count_;
  }

 private:
  enum class RepeatState : std::uint8_t { Idle = 0, Waiting, Repeating };

  struct KeystrokeState {
    std::uint64_t buttons{};
    RepeatState repeat_state{RepeatState::Idle};
    std::uint8_t repeat_button{};
    std::uint64_t repeat_time{};
  };

  [[nodiscard]] std::uint64_t analog_to_keyfield(
      const GamepadState& state) const noexcept;
  [[nodiscard]] Result refresh_cache();

  std::unique_ptr<SdlHost> host_{};
  SdlInputOptions options_{};
  std::vector<SdlHostDevice> devices_{};
  std::vector<std::pair<NativeDeviceId, KeystrokeState>> keystrokes_{};
  int loaded_mapping_count_{};
  bool setup_{};
};

// Returns an SDL2-backed host when Xenon was built with SDL2, otherwise null.
[[nodiscard]] std::unique_ptr<SdlHost> create_sdl2_host();
[[nodiscard]] std::unique_ptr<SdlHost> create_sdl3_host();
[[nodiscard]] std::unique_ptr<SdlHost> create_preferred_sdl_host();
[[nodiscard]] std::unique_ptr<InputDriver> create_sdl_input_driver(
    SdlInputOptions options = {});

}  // namespace xenon::input
