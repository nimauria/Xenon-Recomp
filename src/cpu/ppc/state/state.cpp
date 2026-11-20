#include "xenon/cpu/state.hpp"

namespace xenon::cpu {

void CpuState::update_cr1_from_fpscr() noexcept {
  std::uint8_t f = 0;
  if (fpscr & fpscr_bits::FX)  f |= 0b1000;
  if (fpscr & fpscr_bits::FEX) f |= 0b0100;
  if (fpscr & fpscr_bits::VX)  f |= 0b0010;
  if (fpscr & fpscr_bits::OX)  f |= 0b0001;
  set_cr_field(1, f);
}

}  // namespace xenon::cpu
