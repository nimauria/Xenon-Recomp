#include <fstream>
#include <iostream>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"

using namespace xenon::cpu;

struct Spec { const char* name; GuestAddress address; std::uint32_t word; };
static std::uint32_t d(std::uint32_t p, unsigned rt, unsigned ra, std::int16_t disp) {
  return p | (rt << 21) | (ra << 16) | std::uint16_t(disp);
}
static std::uint32_t x(std::uint32_t p, unsigned rt, unsigned ra, unsigned rb) {
  return p | (rt << 21) | (ra << 16) | (rb << 11);
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const Spec specs[] = {
      {"xm_store", 0x1000u, d(0x90000000u, 4, 3, 0)},
      {"xm_load", 0x1004u, d(0x80000000u, 5, 3, 0)},
      {"xm_lwarx", 0x1008u, x(0x7C000028u, 6, 0, 3)},
      {"xm_stwcx", 0x100Cu, x(0x7C00012Du, 7, 0, 3)},
      {"xm_dcbz", 0x1010u, x(0x7C0007ECu, 0, 0, 3)},
      {"xm_icbi", 0x1014u, x(0x7C0007ACu, 0, 0, 3)},
      {"xm_sync", 0x1018u, 0x7C0004ACu},
      {"xm_lwsync", 0x101Cu, 0x7C2004ACu},
      {"xm_eieio", 0x1020u, 0x7C0006ACu},
      {"xm_isync", 0x1024u, 0x4C00012Cu},
      {"xm_lswi", 0x1028u, x(0x7C0004AAu, 30, 3, 6)},
      {"xm_stswi", 0x102Cu, x(0x7C0005AAu, 30, 3, 6)},
      {"xm_lvlx", 0x1030u, x(0x7C00040Eu, 1, 0, 3)},
      {"xm_stvlx", 0x1034u, x(0x7C00050Eu, 1, 0, 3)},
  };
  std::ofstream out(argv[1]);
  if (!out) return 3;
  out << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n"
         "#include <limits>\n#include \"xenon/cpu/aot_semantics.hpp\"\n"
         "using namespace xenon::cpu;\n";
  Decoder decoder;
  Lifter lifter;
  backend::CppAotBackend backend;
  for (const auto& spec : specs) {
    const auto decoded = decoder.decode(spec.address, spec.word);
    if (!decoded.valid()) return 4;
    ir::Block block{spec.address, {}};
    ir::Builder builder(block);
    if (!lifter.lift(decoded, builder)) return 5;
    out << backend.emit_function(block, spec.name) << '\n';
  }
}
