#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"

using namespace xenon::cpu;

struct Spec { const char* name; GuestAddress address; std::uint32_t word; };

static std::uint32_t xl(std::uint32_t pattern, unsigned bo, unsigned bi, bool lk) {
  return pattern | ((bo & 31u) << 21) | ((bi & 31u) << 16) | (lk ? 1u : 0u);
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  constexpr GuestAddress a=0x3000u;
  constexpr unsigned bi=5u;
  const std::vector<Spec> specs={
    {"ib_bclr",       a+0x00u, xl(0x4C000020u,20,bi,false)},
    {"ib_bclrl",      a+0x04u, xl(0x4C000020u,20,bi,true)},
    {"ib_bclr_cond",  a+0x08u, xl(0x4C000020u,12,bi,false)},
    {"ib_bclr_ctr",   a+0x0Cu, xl(0x4C000020u,16,bi,false)},
    {"ib_bcctr",      a+0x10u, xl(0x4C000420u,20,bi,false)},
    {"ib_bcctrl",     a+0x14u, xl(0x4C000420u,20,bi,true)},
    {"ib_bcctr_true", a+0x18u, xl(0x4C000420u,12,bi,false)},
    {"ib_bcctr_false",a+0x1Cu, xl(0x4C000420u, 4,bi,false)},
  };

  std::ofstream out(argv[1]); if(!out) return 3;
  out << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n";
  out << "#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";
  Decoder decoder; Lifter lifter; backend::CppAotBackend backend;
  for (const auto& s:specs) {
    auto di=decoder.decode(s.address,s.word);
    if(!di.valid()) { std::cerr<<"decode failed "<<s.name<<"\n"; return 4; }
    ir::Block block{s.address,{}}; ir::Builder b(block);
    if(!lifter.lift(di,b)) { std::cerr<<"lift failed "<<s.name<<"\n"; return 5; }
    out << backend.emit_function(block,s.name) << "\n";
  }
  return 0;
}
