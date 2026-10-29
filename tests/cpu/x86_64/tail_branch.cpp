#include <cassert>
#include <cstdint>
#include <iostream>

#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/runtime.hpp"

using namespace xenon::cpu;

ExecutionResult tail_caller_v2(ExecutionContext&);
ExecutionResult tail_target_v2(ExecutionContext&);

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

namespace {
unsigned g_lookups{};

NativeCompiledEntry lookup(void*, ExecutionContext&, GuestAddress target, CompiledLookupKind) {
  ++g_lookups;
  return target == 0xA000u ? &tail_target_v2 : nullptr;
}

NativeCompiledEntry lookup_nothing(void*, ExecutionContext&, GuestAddress, CompiledLookupKind) {
  return nullptr;
}
}  // namespace

int main() {
  FlatMemory memory(0x1000u, 0);
  RecordingRuntime runtime;

  // The callee's `b 0xA000` ran the target, which returned to the caller's
  // own call site; the caller then finished natively. Before, the Branch
  // unwound the caller (r4 never incremented), leaving the dispatcher to
  // resume it mid-function in the interpreter.
  {
    CpuState state{};
    state.gpr[3] = 10;
    state.gpr[4] = 20;
    state.gpr[5] = 30;
    ExecutionContext context(state, memory, runtime);
    context.compiled_lookup = &lookup;
    const auto result = tail_caller_v2(context);
    assert(g_lookups == 1u);
    assert(runtime.calls == 0u);
    assert(state.gpr[5] == 31u);  // tail branching function ran
    assert(state.gpr[3] == 11u);  // tail target ran
    assert(state.gpr[4] == 21u);  // caller resumed after its call
    assert(result.reason == FlowReason::Return);
    assert(result.next_address == 0x8004u);
  }

  // A tail target with no compiled entry still goes to the dispatcher.
  {
    CpuState state{};
    state.gpr[4] = 20;
    ExecutionContext context(state, memory, runtime);
    context.compiled_lookup = &lookup_nothing;
    const auto result = tail_caller_v2(context);
    assert(result.reason == FlowReason::Branch);
    assert(result.next_address == 0xA000u);
    assert(state.gpr[4] == 20u);
  }

  std::cout << "xenon_cpu_tail_branch: ok\n";
  return 0;
}
