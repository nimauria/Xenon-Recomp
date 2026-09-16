#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"

using namespace xenon::cpu;
struct Spec { const char* name; const char* mnemonic; GuestAddress address; std::uint32_t base; };

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::vector<Spec> specs = {
    {"vmem_lvx", "lvx", 0x4000u, 0x7C0000CEu},
    {"vmem_stvx", "stvx", 0x4004u, 0x7C0001CEu},
    {"vmem_lvlx", "lvlx", 0x4008u, 0x7C00040Eu},
    {"vmem_lvrx", "lvrx", 0x400Cu, 0x7C00044Eu},
    {"vmem_stvlx", "stvlx", 0x4010u, 0x7C00050Eu},
    {"vmem_stvrx", "stvrx", 0x4014u, 0x7C00054Eu},
    {"vmem_lvebx", "lvebx", 0x4018u, 0x7C00000Eu},
    {"vmem_lvehx", "lvehx", 0x401Cu, 0x7C00004Eu},
    {"vmem_lvewx", "lvewx", 0x4020u, 0x7C00008Eu},
    {"vmem_lvewx128", "lvewx128", 0x4024u, 0x10000083u},
    {"vmem_stvebx", "stvebx", 0x4028u, 0x7C00010Eu},
    {"vmem_stvehx", "stvehx", 0x402Cu, 0x7C00014Eu},
    {"vmem_stvewx", "stvewx", 0x4030u, 0x7C00018Eu},
    {"vmem_lvsl", "lvsl", 0x4034u, 0x7C00000Cu},
    {"vmem_lvsr", "lvsr", 0x4038u, 0x7C00004Cu},
  };
  std::ofstream out(argv[1]); if (!out) return 3;
  out << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n"
         "#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";
  Decoder decoder; Lifter lifter; backend::CppAotBackend backend;
  for (const auto& spec : specs) {
    // v1, r0, r3 -- RA=0 exercises the architectural zero-base rule.
    const auto word = spec.base | (1u << 21) | (3u << 11);
    auto di = decoder.decode(spec.address, word);
    if (!di.valid() || di.mnemonic() != spec.mnemonic) {
      std::cerr << "decode failed " << spec.name << " got " << di.mnemonic() << "\n";
      return 4;
    }
    ir::Block block{spec.address,{}}; ir::Builder builder(block);
    if (!lifter.lift(di,builder)) return 5;
    out << backend.emit_function(block,spec.name) << "\n";
  }
  return 0;
}
