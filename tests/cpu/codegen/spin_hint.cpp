// `or rN,rN,rN` thread-priority hints become a host spin-wait hint; every
// other `or` stays plain register arithmetic.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"

using namespace xenon::cpu;

namespace {

// or rA,rS,rB (Rc in bit 0): 0x7C000378 | rS<<21 | rA<<16 | rB<<11.
constexpr std::uint32_t or_word(unsigned s, unsigned a, unsigned b, bool rc = false) {
  return 0x7C000378u | (s << 21) | (a << 16) | (b << 11) | (rc ? 1u : 0u);
}

std::string emit(std::uint32_t word) {
  Decoder decoder;
  Lifter lifter;
  const auto insn = decoder.decode(0x82000000u, word);
  if (!insn.valid() || insn.mnemonic() != "orx") {
    std::cerr << "decode failed for " << std::hex << word << '\n';
    std::abort();
  }
  ir::Block block{0x82000000u, {}};
  block.end_address = 0x82000004u;
  ir::Builder builder(block);
  if (!lifter.lift(insn, builder)) std::abort();
  return backend::CppAotBackend{}.emit_function(block, "spin_hint_case");
}

void expect(std::uint32_t word, bool hint, const char* what) {
  const bool emitted = emit(word).find("aot::spin_hint();") != std::string::npos;
  if (emitted != hint) {
    std::cerr << what << ": spin hint " << (emitted ? "emitted" : "missing") << '\n';
    std::abort();
  }
}

}  // namespace

int main() {
  expect(or_word(1, 1, 1), true, "or 1,1,1 (low priority)");
  expect(or_word(6, 6, 6), true, "or 6,6,6 (medium-low priority)");
  expect(or_word(31, 31, 31), true, "or 31,31,31 (very low priority)");
  // Restoring normal priority, plain moves and real ORs are not waits.
  expect(or_word(2, 2, 2), false, "or 2,2,2 (medium priority)");
  expect(or_word(3, 3, 3), false, "mr r3,r3");
  expect(or_word(31, 30, 31), false, "or r30,r31,r31");
  expect(or_word(31, 31, 30), false, "or r31,r31,r30");
  // The record form updates CR0, so it is not the hint.
  expect(or_word(31, 31, 31, true), false, "or. 31,31,31");
  // The hint keeps the register write: r31 still equals itself afterwards.
  const auto source = emit(or_word(31, 31, 31));
  if (source.find("state.gpr[31]") == std::string::npos) {
    std::cerr << "or 31,31,31 lost its register access\n";
    std::abort();
  }
  std::cout << "xenon_cpu_spin_hint: ok\n";
  return 0;
}
