#pragma once

#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/ir.hpp"

namespace xenon::cpu {

class Lifter {
 public:
  // Returns false only for an invalid/unknown machine word. Recognized but not
  // yet expanded operations become compile-time PpcSemantic nodes; native
  // backends are required to reject those until a lowering pass handles them.
  bool lift(const DecodedInstruction& insn, ir::Builder& b) const;

 private:
  bool lift_integer(const DecodedInstruction&, ir::Builder&) const;
  bool lift_control(const DecodedInstruction&, ir::Builder&) const;
  bool lift_memory(const DecodedInstruction&, ir::Builder&) const;
  bool lift_fpu(const DecodedInstruction&, ir::Builder&) const;
  bool lift_vector(const DecodedInstruction&, ir::Builder&) const;
};

}  // namespace xenon::cpu
