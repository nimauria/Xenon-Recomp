#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/runtime.hpp"

using namespace xenon::cpu;
#define FN(n) ExecutionResult n(CpuState&,MemoryPort&,RuntimeServices&)
FN(mem_lbz); FN(mem_lhz); FN(mem_lha); FN(mem_lwz); FN(mem_lwa); FN(mem_ld); FN(mem_lwzu); FN(mem_ldx);
FN(mem_lhbrx); FN(mem_lwbrx); FN(mem_ldbrx); FN(mem_stb); FN(mem_sth); FN(mem_stw); FN(mem_std); FN(mem_stwu);
FN(mem_sthbrx); FN(mem_stwbrx); FN(mem_stdbrx); FN(mem_lfs); FN(mem_lfd); FN(mem_stfs); FN(mem_stfd); FN(mem_stfiwx);
FN(mem_lmw); FN(mem_stmw); FN(mem_lswi); FN(mem_stswi); FN(mem_lswx); FN(mem_stswx); FN(mem_dcbz); FN(mem_dcbz128);
FN(mem_fault_site);
#undef FN

int main(){
  constexpr GuestAddress base=0x8000u;
  FlatMemory mem(0x1000u,base); NullRuntimeServices rt; CpuState s{}; s.gpr[3]=base;

  mem.write8(base+0,0xAB); mem.write16_be(base+2,0xCDEF); mem.write16_be(base+4,0xFF80);
  mem.write32_be(base+8,0x89ABCDEFu); mem.write32_be(base+12,0x80000001u); mem.write64_be(base+16,0x0123456789ABCDEFull);
  mem_lbz(s,mem,rt); mem_lhz(s,mem,rt); mem_lha(s,mem,rt); mem_lwz(s,mem,rt); mem_lwa(s,mem,rt); mem_ld(s,mem,rt);
  assert(s.gpr[10]==0xABu && s.gpr[11]==0xCDEFu);
  assert(s.gpr[12]==0xFFFFFFFFFFFFFF80ull);
  assert(s.gpr[13]==0x89ABCDEFu && s.gpr[14]==0xFFFFFFFF80000001ull && s.gpr[15]==0x0123456789ABCDEFull);

  s.gpr[4]=base+0x20; mem.write32_be(base+0x24,0x13579BDFu); mem_lwzu(s,mem,rt);
  assert(s.gpr[16]==0x13579BDFu && s.gpr[4]==base+0x24);
  s.gpr[5]=0x70; mem.write64_be(base+0x70,0xFEDCBA9876543210ull); mem_ldx(s,mem,rt); assert(s.gpr[17]==0xFEDCBA9876543210ull);

  // Byte-reversed accesses are little-endian relative to normal guest loads/stores.
  s.gpr[5]=0x80; const std::uint8_t seq[8]={1,2,3,4,5,6,7,8}; for(unsigned i=0;i<8;++i)mem.write8(base+0x80+i,seq[i]);
  mem_lhbrx(s,mem,rt); mem_lwbrx(s,mem,rt); mem_ldbrx(s,mem,rt);
  assert(s.gpr[18]==0x0201u && s.gpr[19]==0x04030201u && s.gpr[20]==0x0807060504030201ull);

  s.gpr[21]=0xA1B2C3D4u; s.gpr[22]=0x11223344u; s.gpr[23]=0x55667788u; s.gpr[24]=0x0123456789ABCDEFull;
  mem_stb(s,mem,rt); mem_sth(s,mem,rt); mem_stw(s,mem,rt); mem_std(s,mem,rt);
  assert(mem.read8(base+24)==0xD4u); assert(mem.read16_be(base+26)==0x3344u); assert(mem.read32_be(base+28)==0x55667788u); assert(mem.read64_be(base+32)==0x0123456789ABCDEFull);
  s.gpr[6]=base+0x90; s.gpr[25]=0xDEADBEEFu; mem_stwu(s,mem,rt); assert(s.gpr[6]==base+0x94 && mem.read32_be(base+0x94)==0xDEADBEEFu);

  s.gpr[5]=0xA0; s.gpr[26]=0xA1B2u; s.gpr[27]=0x11223344u; s.gpr[28]=0x0102030405060708ull;
  mem_sthbrx(s,mem,rt); assert(mem.read16_le(base+0xA0)==0xA1B2u);
  mem_stwbrx(s,mem,rt); assert(mem.read32_le(base+0xA0)==0x11223344u);
  mem_stdbrx(s,mem,rt); assert(mem.read64_le(base+0xA0)==0x0102030405060708ull);

  mem.write32_be(base+40,std::bit_cast<std::uint32_t>(1.5f)); mem.write64_be(base+48,std::bit_cast<std::uint64_t>(-2.25));
  mem_lfs(s,mem,rt); mem_lfd(s,mem,rt); assert(s.fpr(1)==1.5 && s.fpr(2)==-2.25);
  s.set_fpr(3,3.75); s.set_fpr(4,-4.5); mem_stfs(s,mem,rt); mem_stfd(s,mem,rt);
  assert(std::bit_cast<float>(mem.read32_be(base+56))==3.75f); assert(std::bit_cast<double>(mem.read64_be(base+64))==-4.5);
  s.fpr_bits[5]=0x11223344AABBCCDDull; s.gpr[5]=0xB0; mem_stfiwx(s,mem,rt); assert(mem.read32_be(base+0xB0)==0xAABBCCDDu);

  for (unsigned i = 0; i < 4; ++i) {
    mem.write32_be(base + 96 + i * 4, 0x10000000u + i);
  }
  s.gpr[28] = s.gpr[29] = s.gpr[30] = s.gpr[31] = ~0ull;
  mem_lmw(s, mem, rt);
  for (unsigned i = 0; i < 4; ++i) {
    assert(s.gpr[28 + i] == 0x10000000u + i);
  }
  for (unsigned i = 0; i < 4; ++i) {
    s.gpr[28 + i] = 0xA0B0C000u + i;
  }
  mem_stmw(s, mem, rt);
  for (unsigned i = 0; i < 4; ++i) {
    assert(mem.read32_be(base + 112 + i * 4) == 0xA0B0C000u + i);
  }

  // Immediate string form: NB=6, bytes fill r30 then high two bytes of r31; remaining bits are zero.
  for (unsigned i = 0; i < 8; ++i) {
    mem.write8(base + i, std::uint8_t(0x10 + i));
  }
  s.gpr[30] = s.gpr[31] = ~0ull;
  mem_lswi(s, mem, rt);
  assert(s.gpr[30] == 0x10111213u);
  assert(s.gpr[31] == 0x14150000u);
  for (unsigned i = 0; i < 8; ++i) {
    mem.write8(base + i, 0);
  }
  s.gpr[30] = 0xA1B2C3D4u;
  s.gpr[31] = 0xE5F60000u;
  mem_stswi(s, mem, rt);
  for(unsigned i=0;i<6;++i){const std::uint8_t expected[6]={0xA1,0xB2,0xC3,0xD4,0xE5,0xF6};assert(mem.read8(base+i)==expected[i]);}

  // Indexed string form takes the count from XER. A zero count performs no load.
  s.gpr[5] = 0xC0;
  for (unsigned i = 0; i < 8; ++i) {
    mem.write8(base + 0xC0 + i, std::uint8_t(0x20 + i));
  }
  s.set_xer_byte_count(6);
  s.gpr[30] = s.gpr[31] = ~0ull;
  mem_lswx(s, mem, rt);
  assert(s.gpr[30]==0x20212223u && s.gpr[31]==0x24250000u);
  s.set_xer_byte_count(0); s.gpr[30]=0xABCDEF0123456789ull; mem_lswx(s,mem,rt); assert(s.gpr[30]==0xABCDEF0123456789ull);
  s.set_xer_byte_count(6); s.gpr[30]=0x01020304u; s.gpr[31]=0x05060000u; mem_stswx(s,mem,rt); for(unsigned i=0;i<6;++i)assert(mem.read8(base+0xC0+i)==std::uint8_t(i+1));

  // Cache block zero aligns down to the architected block size.
  s.gpr[5]=0x12Fu; for(unsigned i=0x120;i<0x140;++i)mem.write8(base+i,0xAA); mem_dcbz(s,mem,rt); for(unsigned i=0x120;i<0x140;++i)assert(mem.read8(base+i)==0);
  s.gpr[5]=0x1FFu; for(unsigned i=0x180;i<0x200;++i)mem.write8(base+i,0xBB); mem_dcbz128(s,mem,rt); for(unsigned i=0x180;i<0x200;++i)assert(mem.read8(base+i)==0);

  s.gpr[3] = 0;
  bool faulted = false;
  try { (void)mem_fault_site(s, mem, rt); }
  catch (const std::out_of_range&) { faulted = true; }
  assert(faulted);
  assert(s.cia == 0x5004u && s.nia == 0x5008u);
  assert(s.gpr[3] == 1u);  // The instruction following the fault did not run.

  std::cout<<"xenon_cpu_scalar_memory: ok\n";
}
