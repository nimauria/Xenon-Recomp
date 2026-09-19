#include "xenon/cpu/lifter.hpp"

namespace xenon::cpu {

bool Lifter::lift(const DecodedInstruction& insn, ir::Builder& b) const {
  if (!insn.valid()) return false;
  b.set_guest(&insn);
  bool lowered = false;
  switch (insn.info->group) {
    case InstructionGroup::Integer: lowered = lift_integer(insn, b); break;
    case InstructionGroup::Control:
    case InstructionGroup::Branch: lowered = lift_control(insn, b); break;
    case InstructionGroup::Memory: lowered = lift_memory(insn, b); break;
    case InstructionGroup::FloatingPoint: lowered = lift_fpu(insn, b); break;
    case InstructionGroup::Vector: lowered = lift_vector(insn, b); break;
  }
  b.set_guest(nullptr);
  return lowered;
}

}  // namespace xenon::cpu
