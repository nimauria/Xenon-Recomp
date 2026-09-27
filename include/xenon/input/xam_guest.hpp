#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "xenon/cpu/external_calls.hpp"
#include "xenon/cpu/memory_port.hpp"
#include "xenon/cpu/state.hpp"
#include "xenon/input/xam_facade.hpp"

namespace xenon::input::xam::guest {

// XAM input ordinals used by Xenia/ReXGlue and current Xbox 360 import tables.
namespace ordinal {
constexpr std::uint32_t XamInputGetCapabilities = 0x00000190u;
constexpr std::uint32_t XamInputGetState = 0x00000191u;
constexpr std::uint32_t XamInputSetState = 0x00000192u;
constexpr std::uint32_t XamInputGetKeystroke = 0x00000193u;
constexpr std::uint32_t XamInputGetKeystrokeEx = 0x00000198u;
constexpr std::uint32_t XamInputGetCapabilitiesEx = 0x000002ADu;
}  // namespace ordinal

struct ExportDescriptor {
  std::uint32_t ordinal{};
  std::string_view name{};
};

[[nodiscard]] std::span<const ExportDescriptor> exports() noexcept;
[[nodiscard]] bool register_exports(cpu::ExternalCallRegistry& registry, class GuestInputBridge& bridge);

// Guest-visible structure sizes and offsets. These are deliberately explicit
// rather than relying on host struct packing so the Xbox big-endian ABI remains
// stable on x86-64 and future ARM64 hosts.
namespace layout {
constexpr std::uint32_t GamepadSize = 12;
constexpr std::uint32_t StateSize = 16;
constexpr std::uint32_t VibrationSize = 4;
constexpr std::uint32_t CapabilitiesSize = 20;
constexpr std::uint32_t KeystrokeSize = 8;

namespace gamepad {
constexpr std::uint32_t Buttons = 0;
constexpr std::uint32_t LeftTrigger = 2;
constexpr std::uint32_t RightTrigger = 3;
constexpr std::uint32_t ThumbLX = 4;
constexpr std::uint32_t ThumbLY = 6;
constexpr std::uint32_t ThumbRX = 8;
constexpr std::uint32_t ThumbRY = 10;
}  // namespace gamepad

namespace state {
constexpr std::uint32_t PacketNumber = 0;
constexpr std::uint32_t Gamepad = 4;
}  // namespace state

namespace capabilities {
constexpr std::uint32_t Type = 0;
constexpr std::uint32_t Subtype = 1;
constexpr std::uint32_t Flags = 2;
constexpr std::uint32_t Gamepad = 4;
constexpr std::uint32_t Vibration = 16;
}  // namespace capabilities

namespace keystroke {
constexpr std::uint32_t VirtualKey = 0;
constexpr std::uint32_t Unicode = 2;
constexpr std::uint32_t Flags = 4;
constexpr std::uint32_t UserIndex = 6;
constexpr std::uint32_t HidCode = 7;
}  // namespace keystroke
}  // namespace layout

namespace capability_flag {
constexpr std::uint16_t ForceFeedbackSupported = 0x0001u;
constexpr std::uint16_t Wireless = 0x0002u;
constexpr std::uint16_t VoiceSupported = 0x0004u;
constexpr std::uint16_t PmdSupported = 0x0008u;
constexpr std::uint16_t NoNavigation = 0x0010u;
}  // namespace capability_flag

class GuestInputBridge {
 public:
  explicit GuestInputBridge(InputSystem& system)
      : system_(system), facade_(system) {}

  [[nodiscard]] XResult get_capabilities(cpu::MemoryPort& memory,
                                         std::uint32_t user_index,
                                         std::uint32_t flags,
                                         cpu::GuestAddress out_caps);
  [[nodiscard]] XResult get_capabilities_ex(cpu::MemoryPort& memory,
                                            std::uint32_t unknown,
                                            std::uint32_t user_index,
                                            std::uint32_t flags,
                                            cpu::GuestAddress out_caps);
  [[nodiscard]] XResult get_state(cpu::MemoryPort& memory,
                                  std::uint32_t user_index,
                                  std::uint32_t flags,
                                  cpu::GuestAddress out_state);
  [[nodiscard]] XResult set_state(cpu::MemoryPort& memory,
                                  std::uint32_t user_index,
                                  std::uint32_t unknown,
                                  cpu::GuestAddress vibration);
  [[nodiscard]] XResult get_keystroke(cpu::MemoryPort& memory,
                                      std::uint32_t user_index,
                                      std::uint32_t flags,
                                      cpu::GuestAddress out_keystroke);
  [[nodiscard]] XResult get_keystroke_ex(cpu::MemoryPort& memory,
                                         cpu::GuestAddress user_index_ptr,
                                         std::uint32_t flags,
                                         cpu::GuestAddress out_keystroke);

  // Minimal PPC export dispatcher. Arguments are read from r3-r10 following
  // the Xbox 360 PPC calling convention and the 32-bit X_RESULT is returned in
  // r3. Unknown ordinals return false and leave CpuState untouched so a future
  // global import/export resolver can chain other modules cleanly.
  [[nodiscard]] bool dispatch(std::uint32_t export_ordinal,
                              cpu::CpuState& state,
                              cpu::MemoryPort& memory);

 private:
  [[nodiscard]] static std::uint32_t normalized_user(std::uint32_t user_index,
                                                     std::uint32_t flags) noexcept;
  [[nodiscard]] std::uint16_t xbox_capability_flags(
      std::uint32_t user_index, std::uint32_t flags,
      const Capabilities& caps) const;

  static void write_gamepad(cpu::MemoryPort& memory, cpu::GuestAddress address,
                            const GamepadState& gamepad);
  static void write_state(cpu::MemoryPort& memory, cpu::GuestAddress address,
                          const State& state);
  static void write_vibration(cpu::MemoryPort& memory,
                              cpu::GuestAddress address,
                              const Vibration& vibration);
  static Vibration read_vibration(cpu::MemoryPort& memory,
                                  cpu::GuestAddress address);
  void write_capabilities(cpu::MemoryPort& memory, cpu::GuestAddress address,
                          std::uint32_t user_index, std::uint32_t flags,
                          const Capabilities& caps) const;
  static void write_keystroke(cpu::MemoryPort& memory,
                              cpu::GuestAddress address,
                              const Keystroke& keystroke);
  [[nodiscard]] static std::uint8_t xbox_subtype(DeviceSubtype subtype) noexcept;

  InputSystem& system_;
  InputFacade facade_;
};

}  // namespace xenon::input::xam::guest
