// The APU XMA register aperture: context array, kick, lock and clear.

#include "audio/xma/xma_internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace xenon::audio {
namespace {

// The APU register file is a little-endian device: byte k of a register sits
// at register address + k, and titles use lwbrx/stwbrx to see true values.
// Memory V2 hands MMIO handlers the big-endian assembly of the accessed
// bytes, so the lanes are mapped here. Treating the registers as big-endian
// gave AC6 a byte-swapped context-array address; it then derived context
// indices near 62000 and kicked registers far past the kick group, so no XMA
// voice ever decoded.
[[nodiscard]] std::uint64_t read_register_lanes(std::uint32_t reg, std::uint32_t byte_in_reg,
                                                std::uint32_t width) noexcept {
  if (byte_in_reg + width > 4u) return 0;
  std::uint64_t value = 0;
  for (std::uint32_t i = 0; i < width; ++i) {
    value = (value << 8u) | ((reg >> ((byte_in_reg + i) * 8u)) & 0xFFu);
  }
  return value;
}

[[nodiscard]] std::uint32_t write_register_lanes(std::uint32_t reg, std::uint32_t byte_in_reg,
                                                 std::uint32_t width,
                                                 std::uint64_t value) noexcept {
  for (std::uint32_t i = 0; i < width; ++i) {
    const auto lane = (byte_in_reg + i) * 8u;
    const auto byte = static_cast<std::uint32_t>(value >> ((width - 1u - i) * 8u)) & 0xFFu;
    reg = (reg & ~(0xFFu << lane)) | (byte << lane);
  }
  return reg;
}

}  // namespace

std::uint64_t XmaDecoder::mmio_read(cpu::GuestAddress address,
                                    std::uint32_t width) {
  if (address < kXmaMmioBase || address >= kXmaMmioBase + kXmaMmioSize ||
      (width != 1u && width != 2u && width != 4u)) {
    return 0;
  }
  const auto offset = static_cast<std::uint32_t>(address - kXmaMmioBase);
  const auto reg_offset = offset & ~3u;
  std::uint32_t value{};
  {
    std::lock_guard lock(mmio_mutex_);
    if (reg_offset == kRegContextArrayAddress) {
      value = context_physical_base_;
    } else if (reg_offset == kRegCurrentContextIndex) {
      value = next_context_index_;
      next_context_index_ = (next_context_index_ + 1u) % kXmaContextCount;
      mmio_registers_[kRegCurrentContextIndex / 4u] = value;
      mmio_registers_[kRegNextContextIndex / 4u] = next_context_index_;
    } else if (reg_offset == kRegNextContextIndex) {
      value = next_context_index_;
    } else {
      value = mmio_registers_[reg_offset / 4u];
    }
  }

  return read_register_lanes(value, offset & 3u, width);
}

void XmaDecoder::mmio_write(cpu::GuestAddress address, std::uint32_t width,
                            std::uint64_t raw_value) {
  if (address < kXmaMmioBase || address >= kXmaMmioBase + kXmaMmioSize ||
      (width != 1u && width != 2u && width != 4u)) {
    return;
  }
  const auto offset = static_cast<std::uint32_t>(address - kXmaMmioBase);
  const auto reg_offset = offset & ~3u;
  const auto byte_in_reg = offset & 3u;
  if (byte_in_reg + width > 4u) return;

  // Narrow writes merge into the register's little-endian byte lanes;
  // commands are dispatched from the resulting dword.
  std::uint32_t value{};
  {
    std::lock_guard lock(mmio_mutex_);
    value = write_register_lanes(mmio_registers_[reg_offset / 4u], byte_in_reg, width,
                                 raw_value);
    // Xenon owns the physical context array. Preserve its address even if a
    // guest probes the hardware register with a write.
    if (reg_offset != kRegContextArrayAddress) {
      mmio_registers_[reg_offset / 4u] = value;
    }
    if (reg_offset == kRegNextContextIndex) {
      next_context_index_ = value % kXmaContextCount;
    }
  }

  const auto dispatch_group = [&](std::uint32_t base, auto&& fn) {
    if (reg_offset < base || reg_offset >= base + kContextRegisterGroupBytes) {
      return false;
    }
    const auto group = (reg_offset - base) / 4u;
    auto bits = value;
    for (std::uint32_t bit = 0; bit < 32u && bits; ++bit, bits >>= 1u) {
      if ((bits & 1u) == 0) continue;
      const auto index = group * 32u + bit;
      if (index >= kXmaContextCount || !allocated_[index].load()) continue;
      const auto context = context_guest_base_ +
          static_cast<cpu::GuestAddress>(index * kXmaContextBytes);
      fn(context);
    }
    return true;
  };

  if (dispatch_group(kRegContextKickBase,
                     [this](cpu::GuestAddress context) {
                       (void)enable_context(context);
                     })) {
    return;
  }
  if (dispatch_group(kRegContextLockBase,
                     [this](cpu::GuestAddress context) {
                       (void)disable_context(context, false);
                     })) {
    return;
  }
  (void)dispatch_group(kRegContextClearBase,
                       [this](cpu::GuestAddress context) {
                         (void)clear_context(context);
                       });
}

}  // namespace xenon::audio
