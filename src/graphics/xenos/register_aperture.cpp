#include "xenon/gpu/register_aperture.hpp"

#include <utility>

namespace xenon::gpu {

namespace {

constexpr std::uint32_t kRegisterCount = kXenosRegisterApertureSize / 4u;

std::uint32_t register_index(memory::GuestAddress address) noexcept {
  return (address & (kXenosRegisterApertureSize - 1u)) / 4u;
}

}  // namespace

XenosRegisterAperture::XenosRegisterAperture(RegisterFile& registers,
                                             WritePointerHook write_pointer_hook)
    : state_(std::make_shared<State>()) {
  state_->registers = &registers;
  state_->write_pointer_hook = std::move(write_pointer_hook);
}

XenosRegisterAperture::~XenosRegisterAperture() {
  state_->active.store(false, std::memory_order_release);
}

std::uint32_t XenosRegisterAperture::read_locked(const State& state, std::uint32_t index) {
  switch (index) {
    case xenos_register::kRbEdramTiming: return xenos_register::kRbEdramTimingValue;
    case xenos_register::kRbBcControl: return xenos_register::kRbBcControlValue;
    case xenos_register::kD1ModeVCounter: return xenos_register::kVCounterValue;
    case xenos_register::kInterruptStatus: return xenos_register::kInterruptStatusVblank;
    case xenos_register::kD1ModeViewportSize: return xenos_register::kViewportSizeValue;
    default: break;
  }
  return state.registers->read(index);
}

void XenosRegisterAperture::write_locked(State& state, std::uint32_t index,
                                         std::uint32_t value) {
  if (index == xenos_register::kCpRbWptr) {
    state.write_pointer_updates.fetch_add(1u, std::memory_order_relaxed);
    if (state.write_pointer_hook) state.write_pointer_hook(value);
  }
  // Like the hardware (and xenia), every store is also latched so it reads back;
  // the fixed-value registers above still report their hardware values.
  static_cast<void>(state.registers->write(index, value));
}

std::uint32_t XenosRegisterAperture::read_register(std::uint32_t index) const {
  if (index >= kRegisterCount) return 0u;
  return read_locked(*state_, index);
}

void XenosRegisterAperture::write_register(std::uint32_t index, std::uint32_t value) {
  if (index >= kRegisterCount) return;
  write_locked(*state_, index, value);
}

bool XenosRegisterAperture::attach(memory::AddressSpace& memory) {
  const std::shared_ptr<State> state = state_;
  // Values are exchanged as the big-endian integer the guest load/store sees.
  // The registers are 32 bits wide; narrower accesses select a byte lane and an
  // 8-byte access spans two consecutive registers.
  auto read = [state](memory::GuestAddress address, std::uint32_t width) -> std::uint64_t {
    if (!state->active.load(std::memory_order_acquire)) return 0u;
    const auto index = register_index(address);
    const auto value = read_locked(*state, index);
    switch (width) {
      case 4: return value;
      case 8: {
        const auto next = index + 1u < kRegisterCount ? read_locked(*state, index + 1u) : 0u;
        return (std::uint64_t{value} << 32u) | next;
      }
      case 2: return (value >> (((address & 2u) == 0u) ? 16u : 0u)) & 0xFFFFu;
      case 1: return (value >> (8u * (3u - (address & 3u)))) & 0xFFu;
      default: return 0u;
    }
  };
  auto write = [state](memory::GuestAddress address, std::uint32_t width,
                       std::uint64_t value) {
    if (!state->active.load(std::memory_order_acquire)) return;
    const auto index = register_index(address);
    switch (width) {
      case 4:
        write_locked(*state, index, static_cast<std::uint32_t>(value));
        return;
      case 8:
        write_locked(*state, index, static_cast<std::uint32_t>(value >> 32u));
        if (index + 1u < kRegisterCount) {
          write_locked(*state, index + 1u, static_cast<std::uint32_t>(value));
        }
        return;
      case 2: {
        const auto shift = ((address & 2u) == 0u) ? 16u : 0u;
        const auto merged = (state->registers->read(index) & ~(0xFFFFu << shift)) |
                            ((static_cast<std::uint32_t>(value) & 0xFFFFu) << shift);
        write_locked(*state, index, merged);
        return;
      }
      case 1: {
        const auto shift = 8u * (3u - (address & 3u));
        const auto merged = (state->registers->read(index) & ~(0xFFu << shift)) |
                            ((static_cast<std::uint32_t>(value) & 0xFFu) << shift);
        write_locked(*state, index, merged);
        return;
      }
      default:
        return;
    }
  };
  return memory.add_mmio_range(kXenosRegisterApertureBase, kXenosRegisterApertureSize,
                               std::move(read), std::move(write), "Xenos registers");
}

}  // namespace xenon::gpu
