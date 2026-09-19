#include <cassert>
#include <cfenv>
#include <cmath>
#include <iostream>

#include "xenon/cpu/aot_semantics.hpp"
#include "xenon/cpu/flat_memory.hpp"

using namespace xenon::cpu;
ExecutionResult smoke_addi3(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_addi4(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_add(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_stw(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_lwz(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_fadd(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_vaddfp(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_vand(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_vandc(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_vor(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_vxor(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_vnor(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_vnor128(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_vand128(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_vsel(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_vsel128(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_lwarx(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_stwcx(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_bc(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_rldicr(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_addo_rc(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_fneg_rc(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_fcmpu(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult smoke_fcmpo(CpuState&,MemoryPort&,RuntimeServices&);

int main(){
  CpuState s{}; FlatMemory mem(0x2000u,0x200u); NullRuntimeServices rt;
  assert(smoke_addi3(s,mem,rt).reason==FlowReason::Fallthrough);
  smoke_addi4(s,mem,rt); smoke_add(s,mem,rt);
  assert(s.gpr[3]==10 && s.gpr[4]==20 && s.gpr[5]==30);
  s.gpr[6]=0x2040u; smoke_stw(s,mem,rt); assert(mem.read32_be(0x2040u)==30u);
  mem.write32_be(0x2040u,0xA1B2C3D4u); smoke_lwz(s,mem,rt); assert(s.gpr[7]==0xA1B2C3D4u);

  s.set_fpr(1,1.25); s.set_fpr(2,2.5); smoke_fadd(s,mem,rt); assert(std::fabs(s.fpr(3)-3.75)<1e-12);
  const int host_round = std::fegetround();
  assert(std::fesetround(FE_DOWNWARD) == 0);
  s.fpscr = static_cast<std::uint32_t>(FpRoundingMode::TowardPositive);
  s.set_fpr(1, 1.0); s.set_fpr(2, 0x1p-53);
  smoke_fadd(s,mem,rt);
  assert(s.fpr(3) == std::nextafter(1.0, 2.0));
  assert((s.fpscr & (fpscr_bits::XX | fpscr_bits::FI | fpscr_bits::FX)) ==
         (fpscr_bits::XX | fpscr_bits::FI | fpscr_bits::FX));
  assert(std::fegetround() == FE_DOWNWARD);
  s.fpscr = static_cast<std::uint32_t>(FpRoundingMode::Nearest);
  smoke_fadd(s,mem,rt);
  assert(s.fpr(3) == 1.0 && std::fegetround() == FE_DOWNWARD);
  assert(std::fesetround(host_round) == 0);
  Vector128 a{},b{}; for(unsigned i=0;i<4;++i){ a.set_u32_be(i,std::bit_cast<std::uint32_t>(float(i+1))); b.set_u32_be(i,std::bit_cast<std::uint32_t>(float(10+i))); }
  s.vr[1]=a;s.vr[2]=b;smoke_vaddfp(s,mem,rt); for(unsigned i=0;i<4;++i){float z=std::bit_cast<float>(s.vr[3].u32_be(i));assert(z==float(11+2*i));}

  for (unsigned i=0; i<16; ++i) {
    a.bytes[i]=static_cast<std::uint8_t>(0xA0u+i);
    b.bytes[i]=static_cast<std::uint8_t>(0x35u+i);
  }
  s.vr[1]=a; s.vr[2]=b;
  smoke_vand(s,mem,rt);
  for (unsigned i=0; i<16; ++i) assert(s.vr[3].bytes[i] == (a.bytes[i]&b.bytes[i]));
  smoke_vandc(s,mem,rt);
  for (unsigned i=0; i<16; ++i) assert(s.vr[3].bytes[i] == (a.bytes[i]&~b.bytes[i]));
  smoke_vor(s,mem,rt);
  for (unsigned i=0; i<16; ++i) assert(s.vr[3].bytes[i] == (a.bytes[i]|b.bytes[i]));
  smoke_vxor(s,mem,rt);
  for (unsigned i=0; i<16; ++i) assert(s.vr[3].bytes[i] == (a.bytes[i]^b.bytes[i]));
  smoke_vnor(s,mem,rt);
  for (unsigned i=0; i<16; ++i) assert(s.vr[3].bytes[i] == static_cast<std::uint8_t>(~(a.bytes[i]|b.bytes[i])));
  smoke_vnor128(s,mem,rt);
  for (unsigned i=0; i<16; ++i) assert(s.vr[3].bytes[i] == static_cast<std::uint8_t>(~(a.bytes[i]|b.bytes[i])));
  smoke_vand128(s,mem,rt);
  for (unsigned i=0; i<16; ++i) assert(s.vr[3].bytes[i] == (a.bytes[i]&b.bytes[i]));

  Vector128 mask{};
  for (unsigned i=0; i<16; ++i) mask.bytes[i]=static_cast<std::uint8_t>(0xF0u^(i*7u));
  s.vr[1]=a; s.vr[2]=b; s.vr[4]=mask;
  smoke_vsel(s,mem,rt);
  for (unsigned i=0; i<16; ++i)
    assert(s.vr[3].bytes[i] == static_cast<std::uint8_t>((a.bytes[i]&~mask.bytes[i])|(b.bytes[i]&mask.bytes[i])));
  s.vr[3]=a; s.vr[1]=b; s.vr[2]=mask;
  smoke_vsel128(s,mem,rt);
  for (unsigned i=0; i<16; ++i)
    assert(s.vr[3].bytes[i] == static_cast<std::uint8_t>((a.bytes[i]&~mask.bytes[i])|(b.bytes[i]&mask.bytes[i])));

  mem.write32_be(0x2040u,0x11112222u); smoke_lwarx(s,mem,rt); assert(s.gpr[8]==0x11112222u && s.reservation.valid);
  s.gpr[9]=0x33334444u; smoke_stwcx(s,mem,rt); assert(mem.read32_be(0x2040u)==0x33334444u); assert((s.cr_field(0)&0x2u)!=0);

  // Store-conditional must match the address of the reservation and must fail
  // after an intervening write invalidates the memory generation token.
  s.gpr[6]=0x2040u; mem.write32_be(0x2040u,0x11112222u); mem.write32_be(0x2044u,0x55556666u);
  smoke_lwarx(s,mem,rt); s.gpr[6]=0x2044u; s.gpr[9]=0x77778888u; smoke_stwcx(s,mem,rt);
  assert(mem.read32_be(0x2044u)==0x55556666u && (s.cr_field(0)&0x2u)==0);
  s.gpr[6]=0x2040u; smoke_lwarx(s,mem,rt); mem.write8(0x2050u,0xAAu); s.gpr[9]=0x9999AAAAu; smoke_stwcx(s,mem,rt);
  assert(mem.read32_be(0x2040u)==0x11112222u && (s.cr_field(0)&0x2u)==0);

  auto br=smoke_bc(s,mem,rt); assert(br.reason==FlowReason::Branch && br.next_address==0x102Cu);

  // Immediate MD rotate/mask must treat rldicr's field as ME, not MB.
  s.gpr[11]=0x0123456789ABCDEFull;
  smoke_rldicr(s,mem,rt);
  const auto rotated=rotate_left64(s.gpr[11],8);
  assert(s.gpr[10] == (rotated & 0xFFFFFFFF00000000ull));

  // OE+Rc: signed overflow sets OV/SO and CR0 includes SO.
  s.xer=0; s.gpr[13]=0x7FFFFFFFFFFFFFFFull; s.gpr[14]=1;
  smoke_addo_rc(s,mem,rt);
  assert(s.gpr[12]==0x8000000000000000ull);
  assert(s.xer_ov() && s.xer_so());
  assert(s.cr_field(0)==0x9u); // LT | SO

  // Sign/move FP instructions do not modify FPSCR, but Rc snapshots it into CR1.
  s.fpscr=fpscr_bits::OX|fpscr_bits::OE; aot::recompute_fpscr_summaries(s);
  const auto fps_before=s.fpscr; s.set_fpr(1,3.5);
  smoke_fneg_rc(s,mem,rt);
  assert(s.fpr(4)==-3.5 && s.fpscr==fps_before);
  assert(s.cr_field(1)==0x5u); // FX=0,FEX=1,VX=0,OX=1

  const double qnan=std::numeric_limits<double>::quiet_NaN();
  s.fpscr=0; s.set_fpr(1,qnan); s.set_fpr(2,1.0);
  smoke_fcmpu(s,mem,rt);
  assert(s.cr_field(2)==0x1u);
  assert((s.fpscr&(fpscr_bits::VXSNAN|fpscr_bits::VXVC|fpscr_bits::VX))==0);
  s.fpscr=0;
  smoke_fcmpo(s,mem,rt);
  assert(s.cr_field(2)==0x1u);
  assert((s.fpscr&(fpscr_bits::VXVC|fpscr_bits::VX|fpscr_bits::FX))==
         (fpscr_bits::VXVC|fpscr_bits::VX|fpscr_bits::FX));
  std::cout<<"xenon_cpu_native_smoke: ok\n";
  return 0;
}
