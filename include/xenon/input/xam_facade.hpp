#pragma once

#include <cstdint>

#include "xenon/input/system.hpp"

namespace xenon::input::xam {

using XResult = std::uint32_t;

namespace result {
constexpr XResult Success = 0x00000000u;
constexpr XResult BadArguments = 0x000000A0u;
constexpr XResult DeviceNotConnected = 0x0000048Fu;
constexpr XResult FunctionFailed = 0x0000065Bu;
constexpr XResult Empty = 0x000010D2u;
}  // namespace result

constexpr std::uint32_t kFlagGamepad = 0x00000001u;
constexpr std::uint32_t kFlagAnyUser = 1u << 30u;
constexpr std::uint32_t kUserIndexAny = 0x000000FFu;

[[nodiscard]] XResult map_result(Result value) noexcept;

class InputFacade {
 public:
  explicit InputFacade(InputSystem& system) : system_(system) {}

  [[nodiscard]] XResult get_capabilities(std::uint32_t user_index,
                                         std::uint32_t flags,
                                         Capabilities* out_caps);
  [[nodiscard]] XResult get_capabilities_ex(std::uint32_t unknown,
                                            std::uint32_t user_index,
                                            std::uint32_t flags,
                                            Capabilities* out_caps);
  [[nodiscard]] XResult get_state(std::uint32_t user_index,
                                  std::uint32_t flags,
                                  State* out_state);
  [[nodiscard]] XResult set_state(std::uint32_t user_index,
                                  std::uint32_t unknown,
                                  const Vibration* vibration);
  [[nodiscard]] XResult get_keystroke(std::uint32_t user_index,
                                      std::uint32_t flags,
                                      Keystroke* out_keystroke);
  [[nodiscard]] XResult get_keystroke_ex(std::uint32_t& user_index,
                                         std::uint32_t flags,
                                         Keystroke* out_keystroke);

 private:
  [[nodiscard]] static bool gamepad_flags_valid(std::uint32_t flags) noexcept;
  [[nodiscard]] static std::uint32_t normalize_user(std::uint32_t user_index,
                                                    std::uint32_t flags) noexcept;

  InputSystem& system_;
};

}  // namespace xenon::input::xam
