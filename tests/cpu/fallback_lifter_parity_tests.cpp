// Fallback-vs-native instruction parity.
//
// Code the recompiler never discovers runs in the dynamic fallback interpreter,
// and one instruction it cannot execute ends the run. The native (AOT) backend and
// the interpreter are separate implementations, so an instruction can be added to
// one and forgotten in the other - exactly how Ace Combat 6 hit `rldicl` (native
// yes, fallback no). This test sweeps the instruction encoding space, finds every
// distinct instruction the decoder recognizes, and requires that whatever the
// native backend supports the fallback supports too.
//
// The two known-gap lists below are the ONLY exceptions. They must stay accurate in
// both directions: an instruction missing from a list but unsupported fails the
// test (a new gap), and an instruction on a list that IS supported fails it too (a
// stale entry - delete it), so the lists can only shrink as gaps are closed.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "xenon/cpu/decoder.hpp"
#include "xenon/recomp/cpu_coverage.hpp"

using namespace xenon;

namespace {

// Supported natively, deliberately or not yet supported by the fallback.
const std::set<std::string> kKnownFallbackGaps = {
    // (filled in after the first audit run)
};

// Not supported by the native backend (functions using them cannot be compiled).
const std::set<std::string> kKnownAotGaps = {
    // (filled in after the first audit run)
};

struct Representative {
  cpu::DecodedInstruction insn;
};

// Every distinct decodable instruction reachable by varying the primary opcode,
// the 11 low extended-opcode bits (which cover X/XL/XFX/XO/A/MD/MDS/VX forms) and a
// few operand-field fills.
std::vector<cpu::DecodedInstruction> enumerate_instructions() {
  cpu::Decoder decoder;
  std::set<std::string> seen;
  std::vector<cpu::DecodedInstruction> found;
  const std::uint32_t fills[] = {0x00000000u, 0x03FFF800u, 0x00A5A800u, 0x02D5D000u, 0x01234800u};
  for (std::uint32_t opcode = 0; opcode < 64u; ++opcode) {
    for (std::uint32_t low = 0; low < 2048u; ++low) {
      for (const auto fill : fills) {
        const std::uint32_t word = (opcode << 26) | (fill & 0x03FFF800u) | low;
        const auto insn = decoder.decode(0x82000000u, word);
        if (!insn.valid()) continue;
        if (seen.insert(std::string(insn.mnemonic())).second) found.push_back(insn);
      }
    }
  }
  return found;
}

}  // namespace

int main() {
  std::cout << "Sweeping the instruction space for fallback/native parity...\n";
  const auto instructions = enumerate_instructions();
  std::cout << instructions.size() << " distinct instructions decoded\n";
  assert(instructions.size() > 250u && "the sweep should reach the full integer/FP/VMX set");

  std::set<std::string> fallback_gaps;
  std::set<std::string> aot_gaps;
  for (const auto& insn : instructions) {
    const bool aot = recomp::aot_supports_instruction(insn);
    const bool fallback = recomp::fallback_supports_instruction(insn);
    if (!aot) aot_gaps.insert(std::string(insn.mnemonic()));
    // Only an instruction the native path supports and the fallback lacks is a
    // parity break: if the native backend cannot do it either, the function is
    // uncompilable regardless (tracked by the AOT list).
    if (aot && !fallback) fallback_gaps.insert(std::string(insn.mnemonic()));
  }

  std::cout << "instructions the native backend supports but the fallback lacks (" << fallback_gaps.size()
            << "):";
  for (const auto& m : fallback_gaps) std::cout << " " << m;
  std::cout << "\ninstructions the native backend does not support (" << aot_gaps.size() << "):";
  for (const auto& m : aot_gaps) std::cout << " " << m;
  std::cout << "\n";

  bool ok = true;
  for (const auto& m : fallback_gaps) {
    if (!kKnownFallbackGaps.contains(m)) {
      std::cerr << "NEW fallback gap (native supports it, the fallback does not): " << m << "\n";
      ok = false;
    }
  }
  for (const auto& m : kKnownFallbackGaps) {
    if (!fallback_gaps.contains(m)) {
      std::cerr << "STALE fallback allowlist entry (now supported - remove it): " << m << "\n";
      ok = false;
    }
  }
  for (const auto& m : aot_gaps) {
    if (!kKnownAotGaps.contains(m)) {
      std::cerr << "NEW native-backend gap: " << m << "\n";
      ok = false;
    }
  }
  for (const auto& m : kKnownAotGaps) {
    if (!aot_gaps.contains(m)) {
      std::cerr << "STALE native-backend allowlist entry (now supported - remove it): " << m << "\n";
      ok = false;
    }
  }
  assert(ok && "instruction parity changed - see the messages above");
  std::cout << (ok ? "Parity holds.\n" : "Parity BROKEN.\n");
  return ok ? 0 : 1;
}
