#include "xenon/cpu/state.hpp"

namespace xenon::cpu {

std::uint8_t CpuState::cr_field(unsigned field) const noexcept {
  field &= 7u;
  const unsigned shift = (7u - field) * 4u;
  return static_cast<std::uint8_t>((cr >> shift) & 0xFu);
}

void CpuState::set_cr_field(unsigned field, std::uint8_t value) noexcept {
  field &= 7u;
  const unsigned shift = (7u - field) * 4u;
  cr = (cr & ~(0xFu << shift)) | ((static_cast<std::uint32_t>(value) & 0xFu) << shift);
}

bool CpuState::cr_bit(unsigned bit) const noexcept {
  bit &= 31u;
  return (cr & (0x80000000u >> bit)) != 0;
}

void CpuState::set_cr_bit(unsigned bit, bool value) noexcept {
  bit &= 31u;
  const auto mask = 0x80000000u >> bit;
  cr = value ? (cr | mask) : (cr & ~mask);
}

void CpuState::set_xer_ca(bool v) noexcept {
  xer = v ? (xer | xer_bits::CA) : (xer & ~xer_bits::CA);
}

void CpuState::set_xer_ov(bool v) noexcept {
  xer = v ? (xer | xer_bits::OV) : (xer & ~xer_bits::OV);
}

void CpuState::set_xer_so(bool v) noexcept {
  xer = v ? (xer | xer_bits::SO) : (xer & ~xer_bits::SO);
}

void CpuState::set_xer_overflow(bool overflow) noexcept {
  set_xer_ov(overflow);
  if (overflow) {
    set_xer_so(true);
  }
}

void CpuState::update_cr0_signed(std::uint64_t result) noexcept {
  const auto s = static_cast<std::int64_t>(result);
  std::uint8_t f = 0;
  if (s < 0) f |= 0b1000;
  else if (s > 0) f |= 0b0100;
  else f |= 0b0010;
  if (xer_so()) f |= 0b0001;
  set_cr_field(0, f);
}

void CpuState::update_cr1_from_fpscr() noexcept {
  std::uint8_t f = 0;
  if (fpscr & fpscr_bits::FX)  f |= 0b1000;
  if (fpscr & fpscr_bits::FEX) f |= 0b0100;
  if (fpscr & fpscr_bits::VX)  f |= 0b0010;
  if (fpscr & fpscr_bits::OX)  f |= 0b0001;
  set_cr_field(1, f);
}

}  // namespace xenon::cpu
