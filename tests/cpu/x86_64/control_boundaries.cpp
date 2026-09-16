#include <cassert>
#include <cstdint>
#include <iostream>

#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/runtime.hpp"

using namespace xenon::cpu;

ExecutionResult ctrl_blr(CpuState&, MemoryPort&, RuntimeServices&);
ExecutionResult ctrl_blrl(CpuState&, MemoryPort&, RuntimeServices&);
ExecutionResult ctrl_bctr(CpuState&, MemoryPort&, RuntimeServices&);
ExecutionResult ctrl_bctrl(CpuState&, MemoryPort&, RuntimeServices&);
ExecutionResult ctrl_bl(CpuState&, MemoryPort&, RuntimeServices&);
ExecutionResult ctrl_sc(CpuState&, MemoryPort&, RuntimeServices&);
ExecutionResult ctrl_twi(CpuState&, MemoryPort&, RuntimeServices&);
ExecutionResult ctrl_mfspr(CpuState&, MemoryPort&, RuntimeServices&);
ExecutionResult ctrl_mtspr(CpuState&, MemoryPort&, RuntimeServices&);
ExecutionResult ctrl_mftb(CpuState&, MemoryPort&, RuntimeServices&);
ExecutionResult ctrl_mftbu(CpuState&, MemoryPort&, RuntimeServices&);

class RecordingRuntime final : public RuntimeServices {
 public:
  ExecutionResult call(GuestAddress target, CpuState&, MemoryPort&) override {
    ++calls; last_call = target; return {FlowReason::Fallthrough, 0, 0};
  }
  ExecutionResult syscall(std::uint32_t level, CpuState& state, MemoryPort&) override {
    ++syscalls; last_syscall = level; return {FlowReason::Syscall, state.cia + 4u, level};
  }
  ExecutionResult trap(std::uint32_t code, CpuState& state, MemoryPort&) override {
    ++traps; last_trap = code; return {FlowReason::Trap, state.cia, code};
  }
  std::uint64_t read_spr(std::uint32_t spr, const CpuState&) override {
    ++spr_reads; last_spr = spr; return spr_read_value;
  }
  void write_spr(std::uint32_t spr, std::uint64_t value, CpuState&) override {
    ++spr_writes; last_spr = spr; spr_write_value = value;
  }
  std::uint64_t read_time_base(const CpuState&) override {
    ++timebase_reads; return timebase;
  }

  unsigned calls{}, syscalls{}, traps{}, spr_reads{}, spr_writes{}, timebase_reads{};
  GuestAddress last_call{};
  std::uint32_t last_syscall{}, last_trap{}, last_spr{};
  std::uint64_t spr_read_value{0x1122334455667788ull};
  std::uint64_t spr_write_value{};
  std::uint64_t timebase{0xAABBCCDD12345678ull};
};

int main() {
  FlatMemory memory(0x1000u, 0x2000u);
  CpuState state{};
  RecordingRuntime runtime;

  // bclr captures LR and aligns the target.
  state.lr = 0x1237u;
  auto r = ctrl_blr(state, memory, runtime);
  assert(r.reason == FlowReason::Return && r.next_address == 0x1234u);

  // bclrl must call the OLD LR, then update LR to CIA + 4.
  state.lr = 0x4567u;
  r = ctrl_blrl(state, memory, runtime);
  assert(runtime.calls == 1 && runtime.last_call == 0x4567u);
  assert(state.lr == 0x3008u);
  assert(r.reason == FlowReason::Fallthrough);

  // bcctr uses CTR as target but must not decrement it.
  state.ctr = 0x89ABu;
  r = ctrl_bctr(state, memory, runtime);
  assert(r.reason == FlowReason::Branch && r.next_address == 0x89A8u);
  assert(state.ctr == 0x89ABu);

  // bcctrl captures the old CTR for the call and sets LR.
  state.ctr = 0x9ABFu;
  r = ctrl_bctrl(state, memory, runtime);
  assert(runtime.calls == 2 && runtime.last_call == 0x9ABFu);
  assert(state.ctr == 0x9ABFu && state.lr == 0x3010u);
  assert(r.reason == FlowReason::Fallthrough);

  // Direct bl target and LR update.
  r = ctrl_bl(state, memory, runtime);
  assert(runtime.calls == 3 && runtime.last_call == 0x3200u);
  assert(state.lr == 0x3104u && r.reason == FlowReason::Fallthrough);

  r = ctrl_sc(state, memory, runtime);
  assert(runtime.syscalls == 1 && runtime.last_syscall == 7u);
  assert(r.reason == FlowReason::Syscall && r.detail == 7u);

  state.gpr[3] = 42;
  r = ctrl_twi(state, memory, runtime);
  assert(runtime.traps == 1 && runtime.last_trap == 4u);
  assert(r.reason == FlowReason::Trap);
  state.gpr[3] = 41;
  r = ctrl_twi(state, memory, runtime);
  assert(runtime.traps == 1 && r.reason == FlowReason::Fallthrough);

  r = ctrl_mfspr(state, memory, runtime);
  assert(runtime.spr_reads == 1 && runtime.last_spr == 500u);
  assert(state.gpr[5] == runtime.spr_read_value);
  state.gpr[6] = 0xCAFEBABEDEADBEEFull;
  r = ctrl_mtspr(state, memory, runtime);
  assert(runtime.spr_writes == 1 && runtime.last_spr == 500u);
  assert(runtime.spr_write_value == state.gpr[6]);

  // Time base is a runtime-owned guest clock, not host TSC/wall clock.
  state.time_base = 1; // Must be ignored by the recording runtime.
  ctrl_mftb(state, memory, runtime);
  ctrl_mftbu(state, memory, runtime);
  assert(runtime.timebase_reads == 2);
  assert(state.gpr[7] == runtime.timebase);
  assert(state.gpr[8] == (runtime.timebase >> 32));

  std::cout << "xenon_cpu_control_boundaries: ok\n";
  return 0;
}
