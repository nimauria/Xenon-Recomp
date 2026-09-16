#include <cstdint>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"

using namespace xenon::cpu;

namespace {
struct Spec {
  const char* name;
  const char* mnemonic;
  GuestAddress address;
  std::uint32_t word;
};

constexpr std::uint32_t encode_spr(std::uint32_t spr) {
  return ((spr & 0x1Fu) << 5) | ((spr >> 5) & 0x1Fu);
}
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  constexpr std::uint32_t kBoAlways = 20u;
  constexpr std::uint32_t kUnknownSpr = 500u;
  const std::vector<Spec> specs = {
      {"ctrl_blr", "bclrx", 0x3000u, 0x4C000020u | (kBoAlways << 21)},
      {"ctrl_blrl", "bclrx", 0x3004u, 0x4C000020u | (kBoAlways << 21) | 1u},
      {"ctrl_bctr", "bcctrx", 0x3008u, 0x4C000420u | (kBoAlways << 21)},
      {"ctrl_bctrl", "bcctrx", 0x300Cu, 0x4C000420u | (kBoAlways << 21) | 1u},
      {"ctrl_bl", "bx", 0x3100u, 0x48000101u},  // target 0x3200
      {"ctrl_sc", "sc", 0x3110u, 0x44000002u | (7u << 5)},
      {"ctrl_twi", "twi", 0x3114u, 0x0C000000u | (4u << 21) | (3u << 16) | 42u},
      {"ctrl_mfspr", "mfspr", 0x3118u,
       0x7C0002A6u | (5u << 21) | (encode_spr(kUnknownSpr) << 11)},
      {"ctrl_mtspr", "mtspr", 0x311Cu,
       0x7C0003A6u | (6u << 21) | (encode_spr(kUnknownSpr) << 11)},
      {"ctrl_mftb", "mftb", 0x3120u,
       0x7C0002E6u | (7u << 21) | (encode_spr(268u) << 11)},
      {"ctrl_mftbu", "mftb", 0x3124u,
       0x7C0002E6u | (8u << 21) | (encode_spr(269u) << 11)},
  };

  std::ofstream out(argv[1]);
  if (!out) return 3;
  out << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n"
         "#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";

  Decoder decoder;
  Lifter lifter;
  backend::CppAotBackend backend;
  for (const auto& spec : specs) {
    auto di = decoder.decode(spec.address, spec.word);
    if (!di.valid() || di.mnemonic() != spec.mnemonic) {
      std::cerr << "decode failed " << spec.name << " expected " << spec.mnemonic
                << " got " << di.mnemonic() << " word=" << std::hex << spec.word << "\n";
      return 4;
    }
    ir::Block block{spec.address, {}};
    ir::Builder builder(block);
    if (!lifter.lift(di, builder)) {
      std::cerr << "lift failed " << spec.name << "\n";
      return 5;
    }
    out << backend.emit_function(block, spec.name) << "\n";
  }
  return 0;
}
