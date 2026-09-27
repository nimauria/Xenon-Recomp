#include "xenon/cpu/state.hpp"

namespace xenon::cpu {

std::uint8_t CpuState::cr_field(unsigned field) const noexcept {
  field &= 7u;
  const unsigned shift = (7u - field) * 4u;
  return static_cast<std::uint8_t>((cr >> shift) & 0xFu);
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

void CpuState::update_cr1_from_fpscr() noexcept {
  std::uint8_t f = 0;
  if (fpscr & fpscr_bits::FX)  f |= 0b1000;
  if (fpscr & fpscr_bits::FEX) f |= 0b0100;
  if (fpscr & fpscr_bits::VX)  f |= 0b0010;
  if (fpscr & fpscr_bits::OX)  f |= 0b0001;
  set_cr_field(1, f);
}

}  // namespace xenon::cpu
