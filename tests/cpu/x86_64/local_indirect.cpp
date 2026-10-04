#include <cassert>
#include <iostream>

#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/runtime.hpp"

using namespace xenon::cpu;
ExecutionResult local_indirect_branch(CpuState&, MemoryPort&, RuntimeServices&);
ExecutionResult local_indirect_call(CpuState&, MemoryPort&, RuntimeServices&);

int main() {
  FlatMemory memory(0x1000u, 0);
  NullRuntimeServices runtime;

  // bctr to a block of the same function stays in native code and resumes
  // exactly at that block.
  {
    CpuState state{};
    state.lr = 0xCAFEBABCu;
    state.gpr[3] = 1u;
    state.gpr[4] = 0x2014u;
    const auto result = local_indirect_branch(state, memory, runtime);
    assert(state.gpr[5] == 0x55u);
    assert(result.reason == FlowReason::Return);
    assert(result.next_address == (state.lr & ~3ull));
    assert(state.cia == 0x2018u);
  }

  // bctr outside the function is not local: it is handed back as a branch
  // (no compiled lookup or fallback exists here), with nia set to the target.
  {
    CpuState state{};
    state.lr = 0xCAFEBABCu;
    state.gpr[3] = 1u;
    state.gpr[4] = 0x9000u;
    const auto result = local_indirect_branch(state, memory, runtime);
    assert(result.reason == FlowReason::Branch);
    assert(result.next_address == 0x9000u);
    assert(state.nia == 0x9000u);
    assert(state.gpr[5] == 0u);
  }

  // bctrl to a block of the same function is a local call: the block runs,
  // its blr returns to 0x3010, and execution continues there.
  {
    CpuState state{};
    state.lr = 0xCAFEBABCu;
    state.gpr[3] = 1u;
    state.gpr[4] = 0x3014u;
    const auto result = local_indirect_call(state, memory, runtime);
    assert(state.gpr[5] == 0x66u);
    assert(result.reason == FlowReason::Return);
    // The final blr at 0x3010 returns through the LR the bctrl set.
    assert(result.next_address == 0x3010u);
    assert(state.cia == 0x3010u);
  }

  std::cout << "xenon_cpu_local_indirect: ok\n";
  return 0;
}
