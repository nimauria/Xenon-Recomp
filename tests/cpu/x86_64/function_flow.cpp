#include <cassert>
#include <iostream>

#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/runtime.hpp"

using namespace xenon::cpu;
ExecutionResult flow_loop(CpuState&, MemoryPort&, RuntimeServices&);

int main() {
  CpuState state{};
  state.lr = 0xCAFEBABCu;
  FlatMemory memory(0x1000u, 0);
  NullRuntimeServices runtime;

  const auto result = flow_loop(state, memory, runtime);
  assert(state.gpr[3] == 3u);
  assert(state.gpr[4] == 3u);
  assert(result.reason == FlowReason::Return);
  assert(result.next_address == (state.lr & ~3ull));
  // The final guest instruction really executed, rather than an instruction
  // dispatcher returning at the backwards branch.
  assert(state.cia == 0x2014u);
  std::cout << "xenon_cpu_function_flow: ok\n";
  return 0;
}
