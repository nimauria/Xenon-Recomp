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
ExecutionResult ib_bcctr_v2(ExecutionContext&);
ExecutionResult ib_bcctrl_v2(ExecutionContext&);

namespace {
constexpr GuestAddress kBranchNativeTarget = 0x5678u;
constexpr GuestAddress kCallNativeTarget = 0x6788u;
unsigned native_branch_calls{};
unsigned native_call_calls{};

ExecutionResult native_branch_target(ExecutionContext& context) {
  ++native_branch_calls;
  context.state.gpr[9] = 0xBEEFu;
  return {FlowReason::Fallthrough, kBranchNativeTarget + 4u, 0u};
}

ExecutionResult native_call_target(ExecutionContext& context) {
  ++native_call_calls;
  context.state.gpr[10] = 0xCAFEu;
  return {FlowReason::Return, 0x3018u, 0u};
}

NativeCompiledEntry lookup_native(void*, ExecutionContext&, GuestAddress target,
                                  CompiledLookupKind kind) {
  if (kind == CompiledLookupKind::Branch && target == kBranchNativeTarget)
    return native_branch_target;
  if (kind == CompiledLookupKind::Call && target == kCallNativeTarget)
    return native_call_target;
  return nullptr;
}
}  // namespace

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

  // CPU V2 indirect branch dispatch can jump straight to a compiled native
  // entry through the ExecutionContext registry without a RuntimeServices
  // call or guest opcode dispatch.
  s={}; rt.calls=0; s.ctr=0x567Bu;
  ExecutionContext branch_context(s,mem,rt);
  branch_context.compiled_lookup=&lookup_native;
  r=ib_bcctr_v2(branch_context);
  assert(native_branch_calls==1u && s.gpr[9]==0xBEEFu);
  assert(r.reason==FlowReason::Fallthrough && r.next_address==0x567Cu);
  assert(rt.calls==0u);

  // Call lookups are a separate contract so only entries proven safe for a
  // normal LR return are eligible for direct native host-call semantics.
  s={}; rt.calls=0; s.ctr=0x678Bu;
  ExecutionContext call_context(s,mem,rt);
  call_context.compiled_lookup=&lookup_native;
  r=ib_bcctrl_v2(call_context);
  assert(native_call_calls==1u && s.gpr[10]==0xCAFEu);
  assert(s.lr==0x3018u && r.reason==FlowReason::Fallthrough);
  assert(rt.calls==0u);

  std::cout<<"xenon_cpu_indirect_branch: ok\n";
}
