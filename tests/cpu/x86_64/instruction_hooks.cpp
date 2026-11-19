// Runs generated guest functions carrying module instruction hooks.
//
// Instruction hooks are how a game module patches one guest instruction
// without replacing its function (AC6 stores each storage job's operation
// code before JobPoolB::Submit, as its title update does). The optimizer keeps
// guest registers in host temporaries across instructions, so a hook must be
// a full state barrier: it sees every register written before it, and the
// guest sees every register it writes.

#include <array>
#include <cassert>
#include <cstdint>
#include <string>

#include "xenon/cpu/aot_semantics.hpp"
#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/function_compiler.hpp"

using namespace xenon::cpu;

ExecutionResult hooks_observe_v2(ExecutionContext&);
ExecutionResult hooks_jump_v2(ExecutionContext&);
ExecutionResult hooks_return_v2(ExecutionContext&);

namespace {

int observe_calls = 0;
int after_calls = 0;
std::uint64_t observed_r3 = 0;
std::uint64_t observed_r5 = 0;
std::uint64_t observed_r4_after = 0;
std::uint32_t observed_memory = 0;

class NullRuntime final : public RuntimeServices {
 public:
  ExecutionResult call(GuestAddress, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 0}; }
  ExecutionResult syscall(std::uint32_t, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 0}; }
  ExecutionResult trap(std::uint32_t, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 0}; }
  std::uint64_t read_spr(std::uint32_t, const CpuState&) override { return 0; }
  void write_spr(std::uint32_t, std::uint64_t, CpuState&) override {}
  std::uint64_t read_time_base(const CpuState&) override { return 0; }
};

std::string compile_error(std::span<const std::uint32_t> words, const InstructionHook& hook) {
  const FunctionCodeRange range{0x8000u, words};
  const auto result =
      StaticFunctionCompiler{}.compile_ranges(0x8000u, std::span(&range, 1u), {},
                                              std::span(&hook, 1u));
  return result.ok ? std::string() : result.error;
}

}  // namespace

void hook_observe(ExecutionContext& context) {
  ++observe_calls;
  observed_r3 = context.state.gpr[3];
  observed_r5 = context.state.gpr[5];
  observed_memory = context.memory.read32_be(static_cast<GuestAddress>(context.state.gpr[5]) + 4u);
  context.memory.write32_be(static_cast<GuestAddress>(context.state.gpr[5]) + 4u, 0x2u);
  context.state.gpr[3] = 0x55u;
}

void hook_after(ExecutionContext& context) {
  ++after_calls;
  observed_r4_after = context.state.gpr[4];
}

bool hook_jump(ExecutionContext& context) { return context.state.gpr[6] != 0u; }

bool hook_return(ExecutionContext& context) { return context.state.gpr[7] != 0u; }

int main() {
  FlatMemory memory(0x1000u);  // guest 0x0-0xFFF; the job object sits at 0x100
  NullRuntime runtime;

  // Before and after one instruction, with registers flowing both ways.
  {
    CpuState state{};
    state.lr = 0x9000u;
    ExecutionContext context(state, memory, runtime);
    const auto result = hooks_observe_v2(context);
    assert(result.reason == FlowReason::Return && result.next_address == 0x9000u);
    assert(observe_calls == 1 && after_calls == 1);
    assert(observed_r3 == 1u && observed_r5 == 0x100u);  // writes reached the hook
    assert(observed_memory == 0u && memory.read32_be(0x104u) == 0x2u);
    assert(state.gpr[4] == 0x56u);                        // the hook's r3 reached the addi
    assert(observed_r4_after == 0x56u);
  }

  // A true result jumps to a block only the hook reaches.
  for (const std::uint64_t taken : {0u, 1u}) {
    CpuState state{};
    state.lr = 0x9000u;
    state.gpr[6] = taken;
    ExecutionContext context(state, memory, runtime);
    const auto result = hooks_jump_v2(context);
    assert(result.reason == FlowReason::Return && result.next_address == 0x9000u);
    assert(state.gpr[3] == (taken ? 2u : 1u));
  }

  // A true result returns to the link register before the instruction.
  for (const std::uint64_t taken : {0u, 1u}) {
    CpuState state{};
    state.lr = 0x9100u;
    state.gpr[7] = taken;
    ExecutionContext context(state, memory, runtime);
    const auto result = hooks_return_v2(context);
    assert(result.reason == FlowReason::Return && result.next_address == 0x9100u);
    assert(state.gpr[3] == (taken ? 9u : 10u));
  }

  // Hooks that cannot be placed fail the compile instead of being dropped.
  const std::array<std::uint32_t, 2> words = {0x38600001u, 0x4E800020u};  // li r3,1; blr
  assert(compile_error(words, {.address = 0x8000u, .ordinal = 0u}).empty());
  assert(!compile_error(words, {.address = 0x8004u, .ordinal = 0u, .after_instruction = true})
              .empty());  // nothing follows blr
  assert(!compile_error(words, {.address = 0x8010u, .ordinal = 0u}).empty());  // outside
  // An after-hook on a call runs when the callee returns.
  const std::array<std::uint32_t, 2> call = {0x48000101u, 0x4E800020u};  // bl +0x100; blr
  assert(compile_error(call, {.address = 0x8000u, .ordinal = 0u, .after_instruction = true})
             .empty());
  return 0;
}
