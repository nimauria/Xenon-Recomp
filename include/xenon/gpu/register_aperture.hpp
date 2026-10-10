#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "xenon/gpu/register_file.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::gpu {

// The Xenos register aperture: 64 KiB of memory-mapped GPU registers at guest
// address 0x7FC80000 (register index = (address & 0xFFFF) / 4). Titles reach
// the GPU directly through it - they poll the display interrupt status from the
// vsync callback, read scanline counters, and, most importantly, publish
// freshly written ring-buffer commands by storing the new write pointer to
// CP_RB_WPTR. With this range left as plain RAM, the game's stores never reach
// the command processor and its loads see zeroes, so any title that waits on
// the GPU stalls forever (Ace Combat 6 stops right after Vd initialisation).
//
// Behaviour follows xenia's GraphicsSystem::ReadRegister/WriteRegister: a small
// set of registers report fixed hardware values, everything else reads back the
// last value written, and a store to CP_RB_WPTR is forwarded to the command
// processor's write pointer.
constexpr memory::GuestAddress kXenosRegisterApertureBase = 0x7FC80000u;
constexpr std::uint32_t kXenosRegisterApertureSize = 0x10000u;

namespace xenos_register {
constexpr std::uint32_t kCpRbWptr = 0x01C5u;
constexpr std::uint32_t kRbEdramTiming = 0x0F00u;
constexpr std::uint32_t kRbBcControl = 0x0F01u;
constexpr std::uint32_t kD1GrphPrimarySurfaceAddress = 0x1844u;
constexpr std::uint32_t kD1ModeVCounter = 0x194Cu;
constexpr std::uint32_t kInterruptStatus = 0x1951u;
constexpr std::uint32_t kD1ModeViewportSize = 0x1961u;

constexpr std::uint32_t kRbEdramTimingValue = 0x08100748u;
constexpr std::uint32_t kRbBcControlValue = 0x0000200Eu;
constexpr std::uint32_t kVCounterValue = 0x000002D0u;      // 720 lines
constexpr std::uint32_t kInterruptStatusVblank = 1u;
constexpr std::uint32_t kViewportSizeValue = 0x050002D0u;  // 1280x720
}  // namespace xenos_register

class XenosRegisterAperture {
 public:
  // Called with the dword ring-buffer write index the guest stored to CP_RB_WPTR.
  using WritePointerHook = std::function<void(std::uint32_t write_index)>;

  // `registers` must outlive the aperture (it is the GraphicsSystem's register
  // file, shared with the command processor so PM4 register writes and guest MMIO
  // reads observe the same state).
  XenosRegisterAperture(RegisterFile& registers, WritePointerHook write_pointer_hook);
  ~XenosRegisterAperture();
  XenosRegisterAperture(const XenosRegisterAperture&) = delete;
  XenosRegisterAperture& operator=(const XenosRegisterAperture&) = delete;

  // Guest-visible 32-bit register access by register index.
  [[nodiscard]] std::uint32_t read_register(std::uint32_t index) const;
  void write_register(std::uint32_t index, std::uint32_t value);

  // Maps the aperture into `memory`. Returns false if the range is unavailable.
  [[nodiscard]] bool attach(memory::AddressSpace& memory);

  [[nodiscard]] std::uint64_t cp_write_pointer_updates() const noexcept {
    return state_->write_pointer_updates.load(std::memory_order_relaxed);
  }

 private:
  // Shared with the MMIO handler closures, which live as long as the address
  // space and may outlive this object; `active` is cleared on destruction so a
  // late guest access degrades to "no device" instead of touching freed state.
  struct State {
    RegisterFile* registers{};
    WritePointerHook write_pointer_hook{};
    std::atomic<bool> active{true};
    std::atomic<std::uint64_t> write_pointer_updates{0};
  };

  [[nodiscard]] static std::uint32_t read_locked(const State& state, std::uint32_t index);
  static void write_locked(State& state, std::uint32_t index, std::uint32_t value);

  std::shared_ptr<State> state_;
};

}  // namespace xenon::gpu
