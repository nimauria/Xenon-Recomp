#include "xenon/input/xam_facade.hpp"

namespace xenon::input::xam {

XResult map_result(Result value) noexcept {
  switch (value) {
    case Result::Success: return result::Success;
    case Result::DeviceNotConnected: return result::DeviceNotConnected;
    case Result::BadArguments: return result::BadArguments;
    case Result::Empty: return result::Empty;
    case Result::Unsupported:
    case Result::Failed:
      return result::FunctionFailed;
  }
  return result::FunctionFailed;
}

bool InputFacade::gamepad_flags_valid(std::uint32_t flags) noexcept {
  return (flags & 0xFFu) == 0 || (flags & kFlagGamepad) != 0;
}

std::uint32_t InputFacade::normalize_user(std::uint32_t user_index,
                                          std::uint32_t flags) noexcept {
  if ((user_index & 0xFFu) == 0xFFu || (flags & kFlagAnyUser) != 0) {
    // Match the Xenia/ReXGlue XAM boundary used by current recomp projects.
    return 0;
  }
  return user_index;
}

XResult InputFacade::get_capabilities(std::uint32_t user_index,
                                      std::uint32_t flags,
                                      Capabilities* out_caps) {
  if (!out_caps) return result::BadArguments;
  if (!gamepad_flags_valid(flags)) return result::DeviceNotConnected;
  return map_result(system_.get_capabilities(normalize_user(user_index, flags),
                                              flags, *out_caps));
}

XResult InputFacade::get_capabilities_ex(std::uint32_t unknown,
                                         std::uint32_t user_index,
                                         std::uint32_t flags,
                                         Capabilities* out_caps) {
  static_cast<void>(unknown);
  return get_capabilities(user_index, flags, out_caps);
}

XResult InputFacade::get_state(std::uint32_t user_index, std::uint32_t flags,
                               State* out_state) {
  if (!gamepad_flags_valid(flags)) return result::DeviceNotConnected;
  State temporary{};
  auto& state = out_state ? *out_state : temporary;
  return map_result(system_.get_state(normalize_user(user_index, flags), state));
}

XResult InputFacade::set_state(std::uint32_t user_index, std::uint32_t unknown,
                               const Vibration* vibration) {
  static_cast<void>(unknown);
  if (!vibration) return result::BadArguments;
  return map_result(system_.set_vibration(normalize_user(user_index, 0),
                                           *vibration));
}

XResult InputFacade::get_keystroke(std::uint32_t user_index,
                                   std::uint32_t flags,
                                   Keystroke* out_keystroke) {
  if (!out_keystroke) return result::BadArguments;
  if (!gamepad_flags_valid(flags)) return result::DeviceNotConnected;
  return map_result(system_.get_keystroke(normalize_user(user_index, flags),
                                           flags, *out_keystroke));
}

XResult InputFacade::get_keystroke_ex(std::uint32_t& user_index,
                                      std::uint32_t flags,
                                      Keystroke* out_keystroke) {
  if (!out_keystroke) return result::BadArguments;
  if (!gamepad_flags_valid(flags)) return result::DeviceNotConnected;
  const auto normalized = normalize_user(user_index, flags);
  const auto mapped = map_result(system_.get_keystroke(normalized, flags,
                                                        *out_keystroke));
  if (mapped == result::Success) user_index = out_keystroke->user_index;
  return mapped;
}

}  // namespace xenon::input::xam
