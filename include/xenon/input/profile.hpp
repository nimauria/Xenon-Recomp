#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <functional>
#include <vector>

#include "xenon/input/types.hpp"

namespace xenon::input {

struct AxisCalibration {
  std::int16_t minimum{-32768};
  std::int16_t center{};
  std::int16_t maximum{32767};
};

struct StickProfile {
  AxisCalibration x{};
  AxisCalibration y{};
  float inner_deadzone{0.0f};
  float outer_deadzone{0.0f};
  float response_exponent{1.0f};
  float sensitivity{1.0f};
  bool invert_x{};
  bool invert_y{};
};

struct TriggerCalibration {
  std::uint8_t minimum{};
  std::uint8_t maximum{255};
};

struct TriggerProfile {
  TriggerCalibration calibration{};
  float inner_deadzone{0.0f};
  float outer_deadzone{0.0f};
  float response_exponent{1.0f};
  float sensitivity{1.0f};
  bool invert{};
};

struct InputProfile {
  std::string id{"default"};
  std::string name{"Default"};
  bool enabled{true};
  StickProfile left_stick{};
  StickProfile right_stick{};
  TriggerProfile left_trigger{};
  TriggerProfile right_trigger{};
};

struct ProfileDiagnostics {
  std::size_t profile_count{};
  std::size_t user_binding_count{};
  std::size_t device_binding_count{};
  std::string default_profile_id{};
  std::string last_error{};
};

[[nodiscard]] bool validate_profile(const InputProfile& profile,
                                    std::string* out_error = nullptr);
[[nodiscard]] GamepadState apply_profile(const InputProfile& profile,
                                         const GamepadState& state);

class ProfileStore {
 public:
  ProfileStore();

  [[nodiscard]] bool upsert(InputProfile profile);
  [[nodiscard]] bool erase(std::string_view id);
  [[nodiscard]] std::optional<InputProfile> profile(std::string_view id) const;
  [[nodiscard]] std::vector<InputProfile> profiles() const;
  [[nodiscard]] std::optional<InputProfile> resolve(
      std::uint32_t user_index, std::string_view device_identity) const;
  // Applies the currently resolved profile without copying it. This is the
  // hot-path entry used by InputSystem state polling.
  void apply(std::uint32_t user_index, std::string_view device_identity,
             GamepadState& state) const;

  [[nodiscard]] bool set_default(std::string_view profile_id);
  [[nodiscard]] bool bind_user(std::uint32_t user_index,
                               std::string_view profile_id);
  [[nodiscard]] bool clear_user(std::uint32_t user_index);
  [[nodiscard]] bool bind_device(std::string device_identity,
                                 std::string_view profile_id);
  [[nodiscard]] bool clear_device(std::string_view device_identity);

  [[nodiscard]] std::optional<std::string> user_binding(
      std::uint32_t user_index) const;
  [[nodiscard]] std::optional<std::string> device_binding(
      std::string_view device_identity) const;

  [[nodiscard]] std::string serialize() const;
  [[nodiscard]] bool deserialize(std::string_view text);
  [[nodiscard]] bool save(const std::filesystem::path& path) const;
  [[nodiscard]] bool load(const std::filesystem::path& path);
  [[nodiscard]] ProfileDiagnostics diagnostics() const;

 private:
  struct StringHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
      return std::hash<std::string_view>{}(value);
    }
  };

  using ProfileMap = std::unordered_map<std::string, InputProfile,
                                        StringHash, std::equal_to<>>;
  using BindingMap = std::unordered_map<std::string, std::string,
                                        StringHash, std::equal_to<>>;

  [[nodiscard]] const InputProfile* resolve_locked(
      std::uint32_t user_index, std::string_view device_identity) const;
  void set_error_locked(std::string message) const;

  mutable std::mutex mutex_{};
  ProfileMap profiles_{};
  std::array<std::string, kMaxUsers> user_bindings_{};
  BindingMap device_bindings_{};
  std::string default_profile_id_{"default"};
  mutable std::string last_error_{};
};

}  // namespace xenon::input
