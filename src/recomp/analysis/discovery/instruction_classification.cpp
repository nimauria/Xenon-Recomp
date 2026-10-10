#include "recomp/analysis/analysis_internal.hpp"

namespace xenon::recomp::detail {

bool is_terminal(const cpu::DecodedInstruction& instruction) {
  if (!instruction.info ||
      instruction.info->group != cpu::InstructionGroup::Branch ||
      instruction.lk()) {
    return false;
  }
  if (instruction.info->format == cpu::InstructionFormat::I) return true;
  if (instruction.mnemonic() == "bx") return true;

  // Conditional branches end a basic block, not necessarily the containing
  // function's primary range. Only stop linear discovery for XL/B-form
  // branches whose BO encoding ignores both CR and CTR (architecturally
  // unconditional). This lets a parent primary range retain its fallthrough
  // block while a declared FunctionChunk supplies the non-contiguous target.
  if (instruction.mnemonic() == "bcx" || instruction.mnemonic() == "bclrx" ||
      instruction.mnemonic() == "bcctrx") {
    return (instruction.bo() & 0x14u) == 0x14u;
  }
  return false;
}

bool is_conditional_control_transfer(const cpu::DecodedInstruction& instruction) {
  if (!instruction.info || instruction.info->group != cpu::InstructionGroup::Branch) return false;
  if (instruction.info->format == cpu::InstructionFormat::I) return false;
  if (instruction.info->format == cpu::InstructionFormat::B ||
      instruction.info->format == cpu::InstructionFormat::XL) {
    // BO bits 0x14 set means ignore both CR and CTR: architecturally
    // unconditional. Every other B/XL transfer retains a condition.
    return (instruction.bo() & 0x14u) != 0x14u;
  }
  return false;
}

// Part 5 (Recomp Analysis V3): classifies a decode failure using Xenon's own
// opcode catalog rather than collapsing every unrecognized word into one
// generic "invalid-ppc" bucket. A primary (6-bit) opcode with zero cataloged
// entries at all is almost certainly not PPC code (embedded data, padding,
// garbage) - "invalid-ppc". A primary opcode Xenon DOES recognize (real
// cataloged instructions exist under it) whose specific bit pattern still
// doesn't match anything is a genuinely different situation: a real PPC
// instruction family Xenon's decoder does not yet implement this exact
// encoding of - "unsupported-ppc", further narrowed to "unsupported-vmx"
// when that primary opcode's cataloged entries are Vector-group (VMX/
// VMX128's Xbox-specific encoding space is the largest known decoder gap -
// see docs/recomp/RECOMP_ANALYSIS_V3.md). Runs only on the (rare) decode-failure
// path, never in the hot per-instruction loop.
const char* classify_decode_failure(std::uint32_t word) noexcept {
  const auto primary = word >> 26u;
  bool primary_known = false;
  bool primary_is_vector = false;
  for (const auto& opcode : cpu::Decoder::opcode_catalog()) {
    if ((opcode.pattern >> 26u) != primary) continue;
    primary_known = true;
    if (opcode.group == cpu::InstructionGroup::Vector) primary_is_vector = true;
  }
  if (!primary_known) return "invalid-ppc";
  return primary_is_vector ? "unsupported-vmx" : "unsupported-ppc";
}

// Part 4 (Recomp Analysis V3): recognizes a common Xbox 360/PowerPC compiler
// function-prologue opening instruction. Deliberately narrow and
// deliberately never a discovery source on its own - see
// DiscoverySource::PrologueHeuristic's doc comment and
// docs/recomp/RECOMP_ANALYSIS_V3.md's "false-positive prevention" section. Only
// ever consulted for a candidate ALREADY being analyzed via some other
// discovery path (entry/export/unwind/hint/direct call/resolved indirect/
// TLS callback); this never independently scans a data section and
// classifies it as code.
//  - `stwu r1, -N(r1)` / `stwux r1, r1, rB` - grows the stack by adjusting
//    r1 itself, the single most common PPC function-prologue opening
//    instruction.
//  - `mflr r0` (mfspr with SPR 8) - saving the link register, almost always
//    among a non-leaf function's first few instructions.
[[nodiscard]] bool looks_like_function_prologue(const cpu::DecodedInstruction& instruction) noexcept {
  if (!instruction.valid()) return false;
  const auto mnemonic = instruction.mnemonic();
  if ((mnemonic == "stwu" || mnemonic == "stwux") && instruction.rt() == 1u && instruction.ra() == 1u)
    return true;
  if (mnemonic == "mfspr" && instruction.spr() == 8u) return true;
  return false;
}

}  // namespace xenon::recomp::detail
