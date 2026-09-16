#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"

using namespace xenon::cpu;
struct Spec { const char* name; GuestAddress address; std::uint32_t word; };

int main(int argc,char**argv){
  if(argc!=2)return 2;
  std::vector<Spec> specs = {
    {"smoke_addi3",0x1000u,0x3860000Au},
    {"smoke_addi4",0x1004u,0x38800014u},
    {"smoke_add",0x1008u,0x7C000214u|(5u<<21)|(3u<<16)|(4u<<11)},
    {"smoke_stw",0x100Cu,0x90000000u|(5u<<21)|(6u<<16)},
    {"smoke_lwz",0x1010u,0x80000000u|(7u<<21)|(6u<<16)},
    {"smoke_fadd",0x1014u,0xFC00002Au|(3u<<21)|(1u<<16)|(2u<<11)},
    {"smoke_vaddfp",0x1018u,0x1000000Au|(3u<<21)|(1u<<16)|(2u<<11)},
    {"smoke_lwarx",0x101Cu,0x7C000028u|(8u<<21)|(0u<<16)|(6u<<11)},
    {"smoke_stwcx",0x1020u,0x7C00012Du|(9u<<21)|(0u<<16)|(6u<<11)},
    {"smoke_bc",0x1024u,0x40000000u|(20u<<21)|(0u<<16)|0x00000008u},
    // rldicr r10,r11,8,31 -- keep architectural bits 0..31 after rotation.
    {"smoke_rldicr",0x1028u,0x78000004u|(11u<<21)|(10u<<16)|(8u<<11)|(31u<<6)},
    // addo. r12,r13,r14 -- exercise OE and Rc together.
    {"smoke_addo_rc",0x102Cu,0x7C000214u|(12u<<21)|(13u<<16)|(14u<<11)|(1u<<10)|1u},
    // fneg. f4,f1 -- result changes, FPSCR does not; CR1 snapshots summaries.
    {"smoke_fneg_rc",0x1030u,0xFC000050u|(4u<<21)|(1u<<11)|1u},
    // fcmpu/fcmpo CR2,f1,f2.
    {"smoke_fcmpu",0x1034u,0xFC000000u|(8u<<21)|(1u<<16)|(2u<<11)},
    {"smoke_fcmpo",0x1038u,0xFC000040u|(8u<<21)|(1u<<16)|(2u<<11)},
  };
  std::ofstream out(argv[1]); if(!out)return 3;
  out<<"#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";
  Decoder d;Lifter l;backend::CppAotBackend be;
  for(auto&s:specs){auto di=d.decode(s.address,s.word);if(!di.valid()){std::cerr<<"decode failed "<<s.name<<" word="<<std::hex<<s.word<<"\n";return 4;}ir::Block b{s.address,{}};ir::Builder ib(b);if(!l.lift(di,ib)){std::cerr<<"lift failed "<<s.name<<" -> "<<di.mnemonic()<<"\n";return 5;}out<<be.emit_function(b,s.name)<<"\n";}
  return 0;
}
