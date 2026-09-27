#include <cassert>
#include <cstdint>
#include <iostream>

#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/runtime.hpp"

using namespace xenon::cpu;

ExecutionResult direct_caller(CpuState&, MemoryPort&, RuntimeServices&);

class RecordingRuntime final : public RuntimeServices {
 public:
  unsigned calls{};

  ExecutionResult call(GuestAddress target, CpuState&, MemoryPort&) override {
    ++calls;
    return {FlowReason::Branch, target, 0};
  }
  ExecutionResult syscall(std::uint32_t level, CpuState& state,
                          MemoryPort&) override {
    return {FlowReason::Syscall, state.cia + 4u, level};
  }
  ExecutionResult trap(std::uint32_t code, CpuState& state,
                       MemoryPort&) override {
    return {FlowReason::Trap, state.cia, code};
  }
  std::uint64_t read_spr(std::uint32_t, const CpuState&) override { return 0; }
  void write_spr(std::uint32_t, std::uint64_t, CpuState&) override {}
  std::uint64_t read_time_base(const CpuState& state) override {
    return state.time_base;
  }
};

int main() {
  CpuState state{};
  state.gpr[3] = 10;
  state.gpr[4] = 20;
  FlatMemory memory(0x1000u, 0);
  RecordingRuntime runtime;

  const auto result = direct_caller(state, memory, runtime);
  assert(runtime.calls == 0u);  // The proven static call never crosses virtual RuntimeServices::call.
  assert(state.gpr[3] == 11u);  // Callee actually executed.
  assert(state.gpr[4] == 21u);  // Caller resumed after the canonical callee return.
  assert(result.reason == FlowReason::Return);
  assert(result.next_address == 0x8004u); // caller did not save its incoming LR in this fixture.

  std::cout << "xenon_cpu_direct_call: ok\n";
  return 0;
}
