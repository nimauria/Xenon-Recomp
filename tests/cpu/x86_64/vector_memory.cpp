#include <cassert>
#include <cstdint>
#include <iostream>

#include "xenon/cpu/aot_semantics.hpp"
#include "xenon/cpu/flat_memory.hpp"

using namespace xenon::cpu;
ExecutionResult vmem_lvx(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult vmem_stvx(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult vmem_lvlx(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult vmem_lvrx(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult vmem_stvlx(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult vmem_stvrx(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult vmem_lvebx(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult vmem_lvehx(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult vmem_lvewx(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult vmem_lvewx128(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult vmem_stvebx(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult vmem_stvehx(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult vmem_stvewx(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult vmem_lvsl(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult vmem_lvsr(CpuState&,MemoryPort&,RuntimeServices&);

static void fill_block(FlatMemory& m, GuestAddress base) {
  for (unsigned i=0;i<16;++i) m.write8(base+i, static_cast<std::uint8_t>(0x80u+i));
}
static Vector128 pattern_vector() {
  Vector128 v{}; for(unsigned i=0;i<16;++i)v.bytes[i]=static_cast<std::uint8_t>(0x10u+i); return v;
}

int main(){
  constexpr GuestAddress base=0x5000u;
  FlatMemory mem(0x100u,base); NullRuntimeServices rt; CpuState s{};
  const auto pattern=pattern_vector();

  // Full loads/stores ignore the low four address bits.
  fill_block(mem,base); s.gpr[3]=base+7; vmem_lvx(s,mem,rt);
  for(unsigned i=0;i<16;++i) assert(s.vr[1].bytes[i]==std::uint8_t(0x80u+i));
  s.vr[1]=pattern; for(unsigned i=0;i<16;++i)mem.write8(base+i,0xEEu);
  s.gpr[3]=base+11; vmem_stvx(s,mem,rt);
  for(unsigned i=0;i<16;++i) assert(mem.read8(base+i)==pattern.bytes[i]);

  for(unsigned n=0;n<16;++n){
    fill_block(mem,base); s.vr[1].bytes.fill(0xCCu); s.gpr[3]=base+n; vmem_lvlx(s,mem,rt);
    for(unsigned i=0;i<16;++i){ const auto expected=i<16-n?std::uint8_t(0x80u+n+i):0u; assert(s.vr[1].bytes[i]==expected); }

    fill_block(mem,base); s.vr[1].bytes.fill(0xCCu); s.gpr[3]=base+n; vmem_lvrx(s,mem,rt);
    for(unsigned i=0;i<16;++i){ const auto expected=i>=16-n?std::uint8_t(0x80u+i-(16-n)):0u; assert(s.vr[1].bytes[i]==expected); }

    s.vr[1]=pattern; for(unsigned i=0;i<16;++i)mem.write8(base+i,0xEEu); s.gpr[3]=base+n; vmem_stvlx(s,mem,rt);
    for(unsigned i=0;i<16;++i){ const auto expected=i<n?0xEEu:pattern.bytes[i-n]; assert(mem.read8(base+i)==expected); }

    s.vr[1]=pattern; for(unsigned i=0;i<16;++i)mem.write8(base+i,0xEEu); s.gpr[3]=base+n; vmem_stvrx(s,mem,rt);
    for(unsigned i=0;i<16;++i){ const auto expected=i<n?pattern.bytes[16-n+i]:0xEEu; assert(mem.read8(base+i)==expected); }
  }

  // Element loads: the addressed element lands in the lane EA[0:3] selects,
  // and, as on the Xbox 360 (xenia-project/xenia PPC test instr_lvexx), every
  // other lane holds the rest of the aligned quadword - not the register's
  // old contents.
  const auto quadword_loaded = [&](const Vector128& v) {
    for (unsigned i = 0; i < 16; ++i)
      if (v.bytes[i] != mem.read8(base + i)) return false;
    return true;
  };
  for (unsigned n = 0; n < 16; ++n) {
    s.vr[1] = pattern;
    mem.write8(base + n, static_cast<std::uint8_t>(0xE0u + n));
    s.gpr[3] = base + n;
    vmem_lvebx(s, mem, rt);
    assert(s.vr[1].bytes[n] == static_cast<std::uint8_t>(0xE0u + n));
    assert(quadword_loaded(s.vr[1]));
  }
  for (unsigned n = 0; n < 16; n += 2) {
    s.vr[1] = pattern;
    mem.write16_be(base + n, static_cast<std::uint16_t>(0xA100u + n));
    s.gpr[3] = base + n + 1;
    vmem_lvehx(s, mem, rt);
    assert(s.vr[1].u16_be(n / 2) == static_cast<std::uint16_t>(0xA100u + n));
    assert(quadword_loaded(s.vr[1]));
  }
  for (unsigned n = 0; n < 16; n += 4) {
    s.vr[1] = pattern;
    mem.write32_be(base + n, 0xA1B20000u + n);
    s.gpr[3] = base + n + 3;
    vmem_lvewx(s, mem, rt);
    assert(s.vr[1].u32_be(n / 4) == 0xA1B20000u + n);
    assert(quadword_loaded(s.vr[1]));
  }
  // The VMX128 word-element form, with its extended VD.
  s.vr[1] = pattern;
  mem.write32_be(base + 12, 0xDEADBEEFu);
  s.gpr[3] = base + 15;
  vmem_lvewx128(s, mem, rt);
  assert(s.vr[1].u32_be(3) == 0xDEADBEEFu);
  assert(quadword_loaded(s.vr[1]));

  // Element stores select the vector element from EA[0:3] and align memory
  // to the element width.
  s.vr[1]=pattern;
  for(unsigned n=0;n<16;++n){ mem.write8(base+n,0); s.gpr[3]=base+n; vmem_stvebx(s,mem,rt); assert(mem.read8(base+n)==pattern.bytes[n]); }
  for(unsigned n=0;n<16;n+=2){ mem.write16_be(base+n,0); s.gpr[3]=base+n+1; vmem_stvehx(s,mem,rt); assert(mem.read16_be(base+n)==pattern.u16_be(n/2)); }
  for(unsigned n=0;n<16;n+=4){ mem.write32_be(base+n,0); s.gpr[3]=base+n+3; vmem_stvewx(s,mem,rt); assert(mem.read32_be(base+n)==pattern.u32_be(n/4)); }

  // lvsl/lvsr are pure EA-derived permutation control vectors.
  for(unsigned n=0;n<16;++n){
    s.gpr[3]=base+n; vmem_lvsl(s,mem,rt);
    auto l=aot::vector_load_shift_left(base+n); assert(s.vr[1].bytes==l.bytes);
    vmem_lvsr(s,mem,rt); auto r=aot::vector_load_shift_right(base+n); assert(s.vr[1].bytes==r.bytes);
  }

  std::cout << "xenon_cpu_vector_memory: ok\n";
  return 0;
}
