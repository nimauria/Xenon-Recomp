#include <cassert>
#include <cstdint>
#include <iostream>

#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/runtime.hpp"

using namespace xenon::cpu;
ExecutionResult ib_bclr(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult ib_bclrl(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult ib_bclr_cond(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult ib_bclr_ctr(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult ib_bcctr(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult ib_bcctrl(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult ib_bcctr_true(CpuState&,MemoryPort&,RuntimeServices&);
ExecutionResult ib_bcctr_false(CpuState&,MemoryPort&,RuntimeServices&);

struct RecordingRuntime final : RuntimeServices {
  GuestAddress last_call{}; unsigned calls{};
  ExecutionResult call(GuestAddress target,CpuState&,MemoryPort&) override {
    last_call=target; ++calls; return {FlowReason::Fallthrough,target,0};
  }
  ExecutionResult syscall(std::uint32_t,CpuState&s,MemoryPort&) override {return {FlowReason::Syscall,s.cia+4,0};}
  ExecutionResult trap(std::uint32_t c,CpuState&s,MemoryPort&) override {return {FlowReason::Trap,s.cia,c};}
  std::uint64_t read_spr(std::uint32_t,const CpuState&) override{return 0;}
  void write_spr(std::uint32_t,std::uint64_t,CpuState&) override{}
  std::uint64_t read_time_base(const CpuState&s) override{return s.time_base;}
};

int main(){
  FlatMemory mem(0x1000u,0); RecordingRuntime rt; CpuState s{};

  // bclr captures the old LR and aligns the target to 4 bytes.
  s.lr=0x1237u; s.ctr=99; auto r=ib_bclr(s,mem,rt);
  assert(r.reason==FlowReason::Return && r.next_address==0x1234u && s.ctr==99u);

  // With LK, the old LR remains the call target while LR gets CIA+4.
  s={}; rt.calls=0; s.lr=0x4567u; r=ib_bclrl(s,mem,rt);
  assert(rt.calls==1 && rt.last_call==0x4567u); // runtime call receives raw guest target
  assert(s.lr==0x3008u && r.reason==FlowReason::Fallthrough);

  // CR-dependent bclr only branches when BI matches BO, with CTR untouched.
  s={}; s.lr=0x2003u; s.ctr=7; s.set_cr_bit(5,false); r=ib_bclr_cond(s,mem,rt);
  assert(r.reason==FlowReason::Fallthrough && s.ctr==7u);
  s.set_cr_bit(5,true); r=ib_bclr_cond(s,mem,rt);
  assert(r.reason==FlowReason::Return && r.next_address==0x2000u);

  // BO=16: ignore CR, decrement CTR and branch while CTR != 0.
  s={}; s.lr=0x4000u; s.ctr=2; r=ib_bclr_ctr(s,mem,rt);
  assert(s.ctr==1u && r.reason==FlowReason::Return && r.next_address==0x4000u);
  s.ctr=1; r=ib_bclr_ctr(s,mem,rt);
  assert(s.ctr==0u && r.reason==FlowReason::Fallthrough);

  // bcctr never decrements CTR for valid BO encodings and aligns jump target.
  s={}; s.ctr=0x567Bu; r=ib_bcctr(s,mem,rt);
  assert(s.ctr==0x567Bu && r.reason==FlowReason::Branch && r.next_address==0x5678u);

  s={}; rt.calls=0; s.ctr=0x678Bu; r=ib_bcctrl(s,mem,rt);
  assert(rt.calls==1 && rt.last_call==0x678Bu);
  assert(s.lr==0x3018u && s.ctr==0x678Bu && r.reason==FlowReason::Fallthrough);

  s={}; s.ctr=0x7003u; s.set_cr_bit(5,true); r=ib_bcctr_true(s,mem,rt);
  assert(r.reason==FlowReason::Branch && r.next_address==0x7000u && s.ctr==0x7003u);
  s.set_cr_bit(5,false); r=ib_bcctr_true(s,mem,rt); assert(r.reason==FlowReason::Fallthrough);
  r=ib_bcctr_false(s,mem,rt); assert(r.reason==FlowReason::Branch && r.next_address==0x7000u);

  std::cout<<"xenon_cpu_indirect_branch: ok\n";
}
