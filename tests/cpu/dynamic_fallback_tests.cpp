#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "xenon/cpu/dynamic_fallback.hpp"
#include "xenon/memory/address_space.hpp"

using namespace xenon::cpu;
using namespace xenon::memory;

namespace {
constexpr GuestAddress kCode = 0x00640000u;
constexpr GuestAddress kReturn = 0x0064F000u;

void write_instruction(AddressSpace& memory, GuestAddress address,
                       std::uint32_t word) {
  memory.write32_be(address, word);
}

void seed_simple_return(AddressSpace& memory) {
  // li r3, 42 ; blr
  write_instruction(memory, kCode + 0u, 0x3860002Au);
  write_instruction(memory, kCode + 4u, 0x4E800020u);
}
}  // namespace

int main() {
  AddressSpace memory;
  assert(memory.initialize());
  assert(memory.commit_fixed(kCode, kBasePageSize, kReadWriteExecute));

  NullRuntimeServices runtime;
  CpuState state{};
  std::vector<DynamicFallbackObservation> observations;
  DynamicFallbackExecutor fallback(
      {}, [&](const DynamicFallbackObservation& observation) {
        observations.push_back(observation);
      });
  ExecutionContext context(state, memory, runtime);
  fallback.bind(context);

  // Unknown-but-executable code can continue through the bounded Gen 7 path.
  seed_simple_return(memory);
  state.lr = kReturn;
  const auto simple =
      context.try_dynamic_fallback(kCode, CompiledLookupKind::Call);
  assert(simple.handled);
  assert(simple.result.reason == FlowReason::Return);
  assert(simple.result.next_address == kReturn);
  assert(state.gpr[3] == 42u);
  assert(fallback.executed_blocks() == 1u);
  assert(fallback.executed_instructions() == 2u);
  assert(observations.size() == 1u);
  assert(observations.front().entry == kCode);
  assert(observations.front().instructions == 2u);
  assert(observations.front().block_fingerprint != 0u);

  // Dynamically discovered callees are recorded independently so later AOT
  // passes learn the actual missing function, not only the outer caller trace.
  const auto observation_count_before_nested = observations.size();
  write_instruction(memory, kCode + 0x00u, 0x7C0802A6u);  // mflr r0
  write_instruction(memory, kCode + 0x04u, 0x4800001Du);  // bl kCode+0x20
  write_instruction(memory, kCode + 0x08u, 0x7C0803A6u);  // mtlr r0
  write_instruction(memory, kCode + 0x0Cu, 0x4E800020u);  // blr
  write_instruction(memory, kCode + 0x20u, 0x38800007u);  // li r4,7
  write_instruction(memory, kCode + 0x24u, 0x4E800020u);  // blr
  state = {};
  state.lr = kReturn;
  const auto nested =
      context.try_dynamic_fallback(kCode, CompiledLookupKind::Call);
  assert(nested.handled);
  assert(nested.result.reason == FlowReason::Return);
  assert(state.gpr[4] == 7u);
  assert(observations.size() == observation_count_before_nested + 2u);
  bool saw_nested_entry = false;
  for (std::size_t i = observation_count_before_nested; i < observations.size(); ++i)
    saw_nested_entry |= observations[i].entry == kCode + 0x20u;
  assert(saw_nested_entry);

  // NX/unmapped addresses are not claimed by the fallback. This leaves import
  // resolution and ordinary failure diagnostics free to handle them.
  const auto non_exec = context.try_dynamic_fallback(
      kCode + kBasePageSize, CompiledLookupKind::Branch);
  assert(!non_exec.handled);

  // PPC trap instructions are conditional. A non-matching twi must fall
  // through; a matching one must delegate to the runtime trap boundary.
  write_instruction(memory, kCode + 0u, 0x38600001u);  // li r3,1
  write_instruction(memory, kCode + 4u, 0x0C830002u);  // twi eq,r3,2
  write_instruction(memory, kCode + 8u, 0x4E800020u);  // blr
  state = {};
  state.lr = kReturn;
  const auto no_trap =
      context.try_dynamic_fallback(kCode, CompiledLookupKind::Call);
  assert(no_trap.handled);
  assert(no_trap.result.reason == FlowReason::Return);

  write_instruction(memory, kCode + 4u, 0x0C830001u);  // twi eq,r3,1
  state = {};
  state.lr = kReturn;
  const auto trap = context.try_dynamic_fallback(kCode, CompiledLookupKind::Call);
  assert(trap.handled);
  assert(trap.result.reason == FlowReason::Trap);

  // A valid executable page containing an unknown PPC word is a handled,
  // diagnosable trap rather than a silent success or an import misclassification.
  write_instruction(memory, kCode, 0x00000000u);
  state.lr = kReturn;
  const auto unsupported =
      context.try_dynamic_fallback(kCode, CompiledLookupKind::Call);
  assert(unsupported.handled);
  assert(unsupported.result.reason == FlowReason::Trap);
  assert(fallback.unsupported_instructions() >= 1u);

  // Guest self-modifying code invalidates the active fallback source snapshot.
  // stw r4,4(r3) rewrites the next instruction on the executable page; the
  // following dispatch must see Memory V2's generation change before fetching it.
  write_instruction(memory, kCode + 0u, 0x90830004u);  // stw r4,4(r3)
  write_instruction(memory, kCode + 4u, 0x4E800020u);  // blr (about to be rewritten)
  state = {};
  state.lr = kReturn;
  state.gpr[3] = kCode;
  state.gpr[4] = 0x60000000u;  // nop
  const auto smc = context.try_dynamic_fallback(kCode, CompiledLookupKind::Call);
  assert(smc.handled);
  assert(smc.result.reason == FlowReason::Trap);
  assert(fallback.source_invalidations() >= 1u);

  // Nested executable calls have their own hard depth guard. Exhausting it is
  // handled by Gen 7 as a trap, never misrouted to RuntimeServices::call.
  DynamicFallbackConfig shallow{};
  shallow.max_nested_calls = 2u;
  DynamicFallbackExecutor call_bounded(shallow);
  ExecutionContext call_bounded_context(state, memory, runtime);
  call_bounded.bind(call_bounded_context);
  write_instruction(memory, kCode, 0x48000001u);  // bl .
  state = {};
  state.lr = kReturn;
  const auto recursion = call_bounded_context.try_dynamic_fallback(
      kCode, CompiledLookupKind::Call);
  assert(recursion.handled);
  assert(recursion.result.reason == FlowReason::Trap);

  // Hard instruction budgets prevent a corrupted/self-looping executable edge
  // from converting the safety net into an unbounded emulator loop.
  DynamicFallbackConfig tight{};
  tight.max_instructions_per_dispatch = 8u;
  DynamicFallbackExecutor bounded(tight);
  ExecutionContext bounded_context(state, memory, runtime);
  bounded.bind(bounded_context);
  write_instruction(memory, kCode, 0x48000000u);  // b .
  state = {};
  state.lr = kReturn;
  const auto loop =
      bounded_context.try_dynamic_fallback(kCode, CompiledLookupKind::Call);
  assert(loop.handled);
  assert(loop.result.reason == FlowReason::Trap);
  assert(bounded.executed_instructions() == 8u);

  std::cout << "dynamic fallback tests passed\n";
  return 0;
}
