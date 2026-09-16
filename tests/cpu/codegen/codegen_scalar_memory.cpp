#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"

using namespace xenon::cpu;
struct Spec { const char* name; const char* mnemonic; GuestAddress address; std::uint32_t word; };

static std::uint32_t d(std::uint32_t p,unsigned rt,unsigned ra,std::int16_t disp){return p|(rt<<21)|(ra<<16)|std::uint16_t(disp);}
static std::uint32_t ds(std::uint32_t p,unsigned rt,unsigned ra,std::int16_t disp){return p|(rt<<21)|(ra<<16)|(std::uint16_t(disp)&0xFFFCu);}
static std::uint32_t x(std::uint32_t p,unsigned rt,unsigned ra,unsigned rb){return p|(rt<<21)|(ra<<16)|(rb<<11);}

int main(int argc,char**argv){
  if (argc != 2) return 2;
  GuestAddress a=0x6000u;
  std::vector<Spec> s={
    {"mem_lbz","lbz",a+=4,d(0x88000000u,10,3,0)},
    {"mem_lhz","lhz",a+=4,d(0xA0000000u,11,3,2)},
    {"mem_lha","lha",a+=4,d(0xA8000000u,12,3,4)},
    {"mem_lwz","lwz",a+=4,d(0x80000000u,13,3,8)},
    {"mem_lwa","lwa",a+=4,ds(0xE8000002u,14,3,12)},
    {"mem_ld","ld",a+=4,ds(0xE8000000u,15,3,16)},
    {"mem_lwzu","lwzu",a+=4,d(0x84000000u,16,4,4)},
    {"mem_ldx","ldx",a+=4,x(0x7C00002Au,17,3,5)},
    {"mem_lhbrx","lhbrx",a+=4,x(0x7C00062Cu,18,3,5)},
    {"mem_lwbrx","lwbrx",a+=4,x(0x7C00042Cu,19,3,5)},
    {"mem_ldbrx","ldbrx",a+=4,x(0x7C000428u,20,3,5)},
    {"mem_stb","stb",a+=4,d(0x98000000u,21,3,24)},
    {"mem_sth","sth",a+=4,d(0xB0000000u,22,3,26)},
    {"mem_stw","stw",a+=4,d(0x90000000u,23,3,28)},
    {"mem_std","std",a+=4,ds(0xF8000000u,24,3,32)},
    {"mem_stwu","stwu",a+=4,d(0x94000000u,25,6,4)},
    {"mem_sthbrx","sthbrx",a+=4,x(0x7C00072Cu,26,3,5)},
    {"mem_stwbrx","stwbrx",a+=4,x(0x7C00052Cu,27,3,5)},
    {"mem_stdbrx","stdbrx",a+=4,x(0x7C000528u,28,3,5)},
    {"mem_lfs","lfs",a+=4,d(0xC0000000u,1,3,40)},
    {"mem_lfd","lfd",a+=4,d(0xC8000000u,2,3,48)},
    {"mem_stfs","stfs",a+=4,d(0xD0000000u,3,3,56)},
    {"mem_stfd","stfd",a+=4,d(0xD8000000u,4,3,64)},
    {"mem_stfiwx","stfiwx",a+=4,x(0x7C0007AEu,5,3,5)},
    {"mem_lmw","lmw",a+=4,d(0xB8000000u,28,3,96)},
    {"mem_stmw","stmw",a+=4,d(0xBC000000u,28,3,112)},
    {"mem_lswi","lswi",a+=4,x(0x7C0004AAu,30,3,6)},
    {"mem_stswi","stswi",a+=4,x(0x7C0005AAu,30,3,6)},
    {"mem_lswx","lswx",a+=4,x(0x7C00042Au,30,3,5)},
    {"mem_stswx","stswx",a+=4,x(0x7C00052Au,30,3,5)},
    {"mem_dcbz","dcbz",a+=4,x(0x7C0007ECu,0,3,5)},
    {"mem_dcbz128","dcbz128",a+=4,x(0x7C2007ECu,0,3,5)},
  };
  std::ofstream out(argv[1]); if(!out)return 3;
  out<<"#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";
  Decoder dec;Lifter lifter;backend::CppAotBackend be;
  for(const auto&e:s){auto di=dec.decode(e.address,e.word);if(!di.valid()||di.mnemonic()!=e.mnemonic){std::cerr<<"decode "<<e.name<<" got "<<di.mnemonic()<<" word="<<std::hex<<e.word<<"\n";return 4;}ir::Block b{e.address,{}};ir::Builder ib(b);if(!lifter.lift(di,ib)){std::cerr<<"lift "<<e.name<<"\n";return 5;}out<<be.emit_function(b,e.name)<<"\n";}
}
