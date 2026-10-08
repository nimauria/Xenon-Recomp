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

  const auto byte_in_reg = offset & 3u;
  if (width == 4u && byte_in_reg == 0) return value;
  if (width == 2u && byte_in_reg <= 2u) {
    const auto shift = (2u - byte_in_reg) * 8u;
    return (value >> shift) & 0xFFFFu;
  }
  if (width == 1u) {
    const auto shift = (3u - byte_in_reg) * 8u;
    return (value >> shift) & 0xFFu;
  }
  return 0;
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
  std::uint32_t value = static_cast<std::uint32_t>(raw_value);

  // Support narrow accesses through the same canonical big-endian register
  // value. Commands are dispatched from the resulting dword.
  if (width != 4u || byte_in_reg != 0) {
    std::lock_guard lock(mmio_mutex_);
    auto merged = mmio_registers_[reg_offset / 4u];
    if (width == 1u) {
      const auto shift = (3u - byte_in_reg) * 8u;
      merged = (merged & ~(0xFFu << shift)) | ((value & 0xFFu) << shift);
    } else if (width == 2u && byte_in_reg <= 2u) {
      const auto shift = (2u - byte_in_reg) * 8u;
      merged = (merged & ~(0xFFFFu << shift)) | ((value & 0xFFFFu) << shift);
    } else {
      return;
    }
    value = merged;
    mmio_registers_[reg_offset / 4u] = merged;
  } else {
    std::lock_guard lock(mmio_mutex_);
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
