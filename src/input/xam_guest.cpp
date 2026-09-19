#include "xenon/input/xam_guest.hpp"

#include <array>

namespace xenon::input::xam::guest {
namespace {

constexpr std::array<ExportDescriptor, 6> kExports{{
    {ordinal::XamInputGetCapabilities, "XamInputGetCapabilities"},
    {ordinal::XamInputGetState, "XamInputGetState"},
    {ordinal::XamInputSetState, "XamInputSetState"},
    {ordinal::XamInputGetKeystroke, "XamInputGetKeystroke"},
    {ordinal::XamInputGetKeystrokeEx, "XamInputGetKeystrokeEx"},
    {ordinal::XamInputGetCapabilitiesEx, "XamInputGetCapabilitiesEx"},
}};

[[nodiscard]] constexpr cpu::GuestAddress guest_ptr(std::uint64_t value) noexcept {
  return static_cast<cpu::GuestAddress>(value & 0xFFFFFFFFu);
}

[[nodiscard]] constexpr std::uint32_t arg32(std::uint64_t value) noexcept {
  return static_cast<std::uint32_t>(value & 0xFFFFFFFFu);
}

}  // namespace

std::span<const ExportDescriptor> exports() noexcept { return kExports; }

bool register_exports(cpu::ExternalCallRegistry& registry, GuestInputBridge& bridge) {
  bool ok = true;
  for (const auto& desc : exports()) {
    ok = registry.register_call(
             "xam", desc.ordinal, std::string(desc.name),
             [&bridge, ordinal = desc.ordinal](cpu::CpuState& state, cpu::MemoryPort& memory) {
               return bridge.dispatch(ordinal, state, memory);
             }) && ok;
  }
  return ok;
}


std::uint32_t GuestInputBridge::normalized_user(std::uint32_t user_index,
                                                std::uint32_t flags) noexcept {
  if ((user_index & 0xFFu) == 0xFFu || (flags & kFlagAnyUser) != 0) return 0;
  return user_index;
}

std::uint8_t GuestInputBridge::xbox_subtype(DeviceSubtype subtype) noexcept {
  switch (subtype) {
    case DeviceSubtype::Gamepad: return 0x01u;
    case DeviceSubtype::Wheel: return 0x02u;
    case DeviceSubtype::ArcadeStick: return 0x03u;
    case DeviceSubtype::FlightStick: return 0x04u;
    case DeviceSubtype::DancePad: return 0x05u;
    case DeviceSubtype::Guitar: return 0x06u;
    case DeviceSubtype::DrumKit: return 0x08u;
    case DeviceSubtype::Unknown: break;
  }
  return 0x00u;
}

std::uint16_t GuestInputBridge::xbox_capability_flags(
    std::uint32_t user_index, std::uint32_t flags,
    const Capabilities& caps) const {
  std::uint16_t result_flags = 0;
  if (caps.flags & CapabilityVibrationSupported) {
    result_flags |= capability_flag::ForceFeedbackSupported;
  }
  if (caps.flags & CapabilityVoiceSupported) {
    result_flags |= capability_flag::VoiceSupported;
  }

  const auto actual_user = normalized_user(user_index, flags);
  if (const auto device_id = system_.device_for_user(actual_user)) {
    if (const auto info = system_.device(*device_id);
        info && info->connection == ConnectionType::Wireless) {
      result_flags |= capability_flag::Wireless;
    }
  }
  return result_flags;
}

void GuestInputBridge::write_gamepad(cpu::MemoryPort& memory,
                                     cpu::GuestAddress address,
                                     const GamepadState& gamepad) {
  memory.write16_be(address + layout::gamepad::Buttons, gamepad.buttons);
  memory.write8(address + layout::gamepad::LeftTrigger, gamepad.left_trigger);
  memory.write8(address + layout::gamepad::RightTrigger, gamepad.right_trigger);
  memory.write16_be(address + layout::gamepad::ThumbLX,
                    static_cast<std::uint16_t>(gamepad.thumb_lx));
  memory.write16_be(address + layout::gamepad::ThumbLY,
                    static_cast<std::uint16_t>(gamepad.thumb_ly));
  memory.write16_be(address + layout::gamepad::ThumbRX,
                    static_cast<std::uint16_t>(gamepad.thumb_rx));
  memory.write16_be(address + layout::gamepad::ThumbRY,
                    static_cast<std::uint16_t>(gamepad.thumb_ry));
}

void GuestInputBridge::write_state(cpu::MemoryPort& memory,
                                   cpu::GuestAddress address,
                                   const State& state) {
  memory.write32_be(address + layout::state::PacketNumber, state.packet_number);
  write_gamepad(memory, address + layout::state::Gamepad, state.gamepad);
}

void GuestInputBridge::write_vibration(cpu::MemoryPort& memory,
                                       cpu::GuestAddress address,
                                       const Vibration& vibration) {
  memory.write16_be(address, vibration.left_motor_speed);
  memory.write16_be(address + 2u, vibration.right_motor_speed);
}

Vibration GuestInputBridge::read_vibration(cpu::MemoryPort& memory,
                                           cpu::GuestAddress address) {
  Vibration value{};
  value.left_motor_speed = memory.read16_be(address);
  value.right_motor_speed = memory.read16_be(address + 2u);
  return value;
}

void GuestInputBridge::write_capabilities(cpu::MemoryPort& memory,
                                          cpu::GuestAddress address,
                                          std::uint32_t user_index,
                                          std::uint32_t flags,
                                          const Capabilities& caps) const {
  memory.write8(address + layout::capabilities::Type,
                caps.type == DeviceType::Gamepad ? 0x01u : 0x00u);
  memory.write8(address + layout::capabilities::Subtype,
                xbox_subtype(caps.subtype));
  memory.write16_be(address + layout::capabilities::Flags,
                    xbox_capability_flags(user_index, flags, caps));
  write_gamepad(memory, address + layout::capabilities::Gamepad, caps.gamepad);
  write_vibration(memory, address + layout::capabilities::Vibration,
                  caps.vibration);
}

void GuestInputBridge::write_keystroke(cpu::MemoryPort& memory,
                                       cpu::GuestAddress address,
                                       const Keystroke& keystroke) {
  memory.write16_be(address + layout::keystroke::VirtualKey,
                    keystroke.virtual_key);
  memory.write16_be(address + layout::keystroke::Unicode,
                    static_cast<std::uint16_t>(keystroke.unicode));
  memory.write16_be(address + layout::keystroke::Flags, keystroke.flags);
  memory.write8(address + layout::keystroke::UserIndex,
                keystroke.user_index);
  memory.write8(address + layout::keystroke::HidCode, keystroke.hid_code);
}

XResult GuestInputBridge::get_capabilities(cpu::MemoryPort& memory,
                                           std::uint32_t user_index,
                                           std::uint32_t flags,
                                           cpu::GuestAddress out_caps) {
  if (!out_caps) return result::BadArguments;
  Capabilities caps{};
  const auto value = facade_.get_capabilities(user_index, flags, &caps);
  if (value == result::Success) {
    write_capabilities(memory, out_caps, user_index, flags, caps);
  }
  return value;
}

XResult GuestInputBridge::get_capabilities_ex(cpu::MemoryPort& memory,
                                              std::uint32_t unknown,
                                              std::uint32_t user_index,
                                              std::uint32_t flags,
                                              cpu::GuestAddress out_caps) {
  if (!out_caps) return result::BadArguments;
  Capabilities caps{};
  const auto value =
      facade_.get_capabilities_ex(unknown, user_index, flags, &caps);
  if (value == result::Success) {
    write_capabilities(memory, out_caps, user_index, flags, caps);
  }
  return value;
}

XResult GuestInputBridge::get_state(cpu::MemoryPort& memory,
                                    std::uint32_t user_index,
                                    std::uint32_t flags,
                                    cpu::GuestAddress out_state) {
  State state{};
  const auto value = facade_.get_state(user_index, flags,
                                       out_state ? &state : nullptr);
  if (value == result::Success && out_state) {
    write_state(memory, out_state, state);
  }
  return value;
}

XResult GuestInputBridge::set_state(cpu::MemoryPort& memory,
                                    std::uint32_t user_index,
                                    std::uint32_t unknown,
                                    cpu::GuestAddress vibration) {
  if (!vibration) return facade_.set_state(user_index, unknown, nullptr);
  const auto value = read_vibration(memory, vibration);
  return facade_.set_state(user_index, unknown, &value);
}

XResult GuestInputBridge::get_keystroke(cpu::MemoryPort& memory,
                                        std::uint32_t user_index,
                                        std::uint32_t flags,
                                        cpu::GuestAddress out_keystroke) {
  if (!out_keystroke) return result::BadArguments;
  Keystroke keystroke{};
  const auto value = facade_.get_keystroke(user_index, flags, &keystroke);
  if (value == result::Success) {
    write_keystroke(memory, out_keystroke, keystroke);
  }
  return value;
}

XResult GuestInputBridge::get_keystroke_ex(cpu::MemoryPort& memory,
                                           cpu::GuestAddress user_index_ptr,
                                           std::uint32_t flags,
                                           cpu::GuestAddress out_keystroke) {
  if (!user_index_ptr || !out_keystroke) return result::BadArguments;
  auto user_index = memory.read32_be(user_index_ptr);
  Keystroke keystroke{};
  const auto value = facade_.get_keystroke_ex(user_index, flags, &keystroke);
  if (value == result::Success) {
    write_keystroke(memory, out_keystroke, keystroke);
    memory.write32_be(user_index_ptr, user_index);
  }
  return value;
}

bool GuestInputBridge::dispatch(std::uint32_t export_ordinal,
                                cpu::CpuState& state,
                                cpu::MemoryPort& memory) {
  XResult value{};
  switch (export_ordinal) {
    case ordinal::XamInputGetCapabilities:
      value = get_capabilities(memory, arg32(state.gpr[3]),
                               arg32(state.gpr[4]), guest_ptr(state.gpr[5]));
      break;
    case ordinal::XamInputGetState:
      value = get_state(memory, arg32(state.gpr[3]), arg32(state.gpr[4]),
                        guest_ptr(state.gpr[5]));
      break;
    case ordinal::XamInputSetState:
      value = set_state(memory, arg32(state.gpr[3]), arg32(state.gpr[4]),
                        guest_ptr(state.gpr[5]));
      break;
    case ordinal::XamInputGetKeystroke:
      value = get_keystroke(memory, arg32(state.gpr[3]), arg32(state.gpr[4]),
                            guest_ptr(state.gpr[5]));
      break;
    case ordinal::XamInputGetKeystrokeEx:
      value = get_keystroke_ex(memory, guest_ptr(state.gpr[3]),
                               arg32(state.gpr[4]), guest_ptr(state.gpr[5]));
      break;
    case ordinal::XamInputGetCapabilitiesEx:
      value = get_capabilities_ex(
          memory, arg32(state.gpr[3]), arg32(state.gpr[4]),
          arg32(state.gpr[5]), guest_ptr(state.gpr[6]));
      break;
    default:
      return false;
  }
  state.gpr[3] = value;
  return true;
}

}  // namespace xenon::input::xam::guest
