#include <cstdint>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"

using namespace xenon::cpu;

struct Spec {
  const char* name;
  const char* mnemonic;
  GuestAddress address;
  std::uint32_t operand_bits;
};

static const OpcodeInfo* find_opcode(std::string_view mnemonic) {
  for (const auto& op : Decoder::opcode_catalog()) {
    if (op.mnemonic == mnemonic) return &op;
  }
  return nullptr;
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::vector<Spec> specs = {
      {"edge_addc", "addcx", 0x6000u, (5u << 21) | (3u << 16) | (4u << 11)},
      {"edge_adde", "addex", 0x6004u, (5u << 21) | (3u << 16) | (4u << 11)},
      {"edge_addme", "addmex", 0x6008u, (5u << 21) | (3u << 16)},
      {"edge_addze", "addzex", 0x600Cu, (5u << 21) | (3u << 16)},
      {"edge_subfc", "subfcx", 0x6010u, (5u << 21) | (3u << 16) | (4u << 11)},
      {"edge_subfe", "subfex", 0x6014u, (5u << 21) | (3u << 16) | (4u << 11)},
      {"edge_neg_o", "negx", 0x6018u, (5u << 21) | (3u << 16) | (1u << 10)},
      {"edge_divd_o", "divdx", 0x601Cu, (5u << 21) | (3u << 16) | (4u << 11) | (1u << 10)},
      {"edge_srawi", "srawix", 0x6020u, (3u << 21) | (5u << 16) | (1u << 11)},
      {"edge_sradi", "sradix", 0x6024u, (3u << 21) | (5u << 16) | (1u << 11)},
      // cmp cr2,0,r3,r4: BF occupies RT[2:4], L=0 word compare.
      {"edge_cmpw", "cmp", 0x6028u, (8u << 21) | (3u << 16) | (4u << 11)},
      // cmpl cr2,1,r3,r4: L=1 doubleword unsigned compare.
      {"edge_cmpld", "cmpl", 0x602Cu, (9u << 21) | (3u << 16) | (4u << 11)},
  };

  std::ofstream out(argv[1]);
  if (!out) return 3;
  out << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n"
         "#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";

  Decoder decoder;
  Lifter lifter;
  backend::CppAotBackend backend;
  for (const auto& spec : specs) {
    const auto* info = find_opcode(spec.mnemonic);
    if (!info) {
      std::cerr << "missing opcode " << spec.mnemonic << "\n";
      return 4;
    }
    const auto word = info->pattern | spec.operand_bits;
    const auto di = decoder.decode(spec.address, word);
    if (!di.valid() || di.mnemonic() != spec.mnemonic) {
      std::cerr << "decode failed " << spec.name << " got " << di.mnemonic() << "\n";
      return 5;
    }
    ir::Block block{spec.address, {}};
    ir::Builder builder(block);
    if (!lifter.lift(di, builder)) return 6;
    out << backend.emit_function(block, spec.name) << "\n";
  }
  return 0;
}
