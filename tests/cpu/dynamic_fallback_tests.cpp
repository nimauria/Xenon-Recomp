#include <bit>
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

// A single recognized import-thunk address; call() records what it was
// invoked with instead of just branching into it like NullRuntimeServices,
// so a test can prove the real import dispatch ran rather than the
// interpreter decoding the thunk's placeholder bytes as PPC.
class ImportThunkRuntimeServices final : public RuntimeServices {
 public:
  explicit ImportThunkRuntimeServices(GuestAddress thunk_address)
      : thunk_address_(thunk_address) {}

  bool is_recognized_import_thunk(GuestAddress target) override {
    return target == thunk_address_;
  }
  ExecutionResult call(GuestAddress target, CpuState& state, MemoryPort&) override {
    ++call_count;
    last_call_target = target;
    state.gpr[3] = 0x99u;
    return {FlowReason::Return, static_cast<GuestAddress>(state.lr), 0u};
  }
  ExecutionResult syscall(std::uint32_t level, CpuState& state, MemoryPort&) override {
    return {FlowReason::Syscall, state.cia + 4u, level};
  }
  ExecutionResult trap(std::uint32_t trap_code, CpuState& state, MemoryPort&) override {
    return {FlowReason::Trap, state.cia, trap_code};
  }
  std::uint64_t read_spr(std::uint32_t, const CpuState&) override { return 0; }
  void write_spr(std::uint32_t, std::uint64_t, CpuState&) override {}
  std::uint64_t read_time_base(const CpuState& state) override { return state.time_base; }

  const GuestAddress thunk_address_;
  int call_count = 0;
  GuestAddress last_call_target = 0;
};
}  // namespace

int main() {
  AddressSpace memory;
  assert(memory.initialize());
  assert(memory.commit_fixed(kCode, kBasePageSize, kReadWriteExecute));
  // A separate, non-executable page for scratch data: sharing kCode's own
  // page for data a test instruction stores to would (correctly) trip the
  // self-modifying-code guard, since a write anywhere on an executing page
  // invalidates that page's dispatch-time fingerprint.
  constexpr GuestAddress kDataPage = kCode + kBasePageSize;
  assert(memory.commit_fixed(kDataPage, kBasePageSize, kReadWrite));

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

  // An UNLINKED tail branch (bcctrx/bctr with LK=0 - real compiler output
  // for e.g. "if (x) return f();") landing on a recognized import-thunk
  // address must be routed through RuntimeServices::call(), never decoded
  // as PPC. Only a linked `bl` into the same address was previously routed
  // correctly (via execute_call() re-entering run(), which re-checks
  // is_recognized_import_thunk() at the top) - a plain tail branch just set
  // next_pc and looped back into the decoder, hitting the thunk's
  // loader-owned placeholder bytes and producing a spurious "unsupported
  // instruction" trap for what is really a perfectly resolvable import call.
  constexpr GuestAddress kThunk = kCode + 0x300u;
  write_instruction(memory, kCode + 0u, 0x4E800420u);  // bctr (unconditional, LK=0)
  write_instruction(memory, kThunk, 0x00000000u);      // real hardware: loader-owned
                                                        // placeholder bytes, never valid PPC
  ImportThunkRuntimeServices thunk_runtime(kThunk);
  CpuState thunk_state{};
  thunk_state.ctr = kThunk;
  thunk_state.lr = kReturn;
  DynamicFallbackExecutor thunk_fallback;
  ExecutionContext thunk_context(thunk_state, memory, thunk_runtime);
  thunk_fallback.bind(thunk_context);
  const auto tail_branch_to_thunk =
      thunk_context.try_dynamic_fallback(kCode, CompiledLookupKind::Call);
  assert(tail_branch_to_thunk.handled);
  assert(tail_branch_to_thunk.result.reason == FlowReason::Return);
  assert(thunk_runtime.call_count == 1 &&
         "the tail branch must reach RuntimeServices::call(), not the PPC decoder");
  assert(thunk_runtime.last_call_target == kThunk);
  assert(thunk_state.gpr[3] == 0x99u);
  assert(thunk_fallback.unsupported_instructions() == 0u &&
         "the thunk's placeholder bytes must never be decoded as PPC");

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

  // Reviewer feedback on the AC6 Runtime Readiness pass's Part 14 fallback
  // accounting: fallback_unique_pc_count()/fallback_hot_pcs() must
  // distinguish "many instructions from a few hot entries" from "the same
  // total spread across many distinct entries" - executed_blocks() alone
  // cannot tell those apart. Uses its own executor/context so it is
  // independent of the shared mutable state exercised above.
  seed_simple_return(memory);
  write_instruction(memory, kCode + 0x100u, 0x38600000u);  // li r3,0
  write_instruction(memory, kCode + 0x104u, 0x4E800020u);  // blr
  DynamicFallbackExecutor telemetry_fallback;
  ExecutionContext telemetry_context(state, memory, runtime);
  telemetry_fallback.bind(telemetry_context);
  assert(telemetry_fallback.fallback_unique_pc_count() == 0u);

  state = {};
  state.lr = kReturn;
  assert(telemetry_context.try_dynamic_fallback(kCode, CompiledLookupKind::Call).handled);
  state = {};
  state.lr = kReturn;
  assert(telemetry_context.try_dynamic_fallback(kCode, CompiledLookupKind::Call).handled);
  state = {};
  state.lr = kReturn;
  assert(telemetry_context
             .try_dynamic_fallback(kCode + 0x100u, CompiledLookupKind::Call)
             .handled);

  assert(telemetry_fallback.fallback_unique_pc_count() == 2u);
  const auto hot_pcs = telemetry_fallback.fallback_hot_pcs(10u);
  assert(hot_pcs.size() == 2u);
  assert(hot_pcs.front().first == kCode);
  assert(hot_pcs.front().second == 2u);
  assert(hot_pcs.back().first == kCode + 0x100u);
  assert(hot_pcs.back().second == 1u);
  const auto top_one = telemetry_fallback.fallback_hot_pcs(1u);
  assert(top_one.size() == 1u);
  assert(top_one.front().first == kCode);

  // Real AC6 repro: the entire multiply/divide family (mulli, mullw*,
  // mulhw*, divw*, and their 64-bit d-suffixed siblings) had no case in
  // execute_simple() at all, so hitting one - even the extremely common
  // "mulli" - fell through to the unsupported-instruction trap
  // (kFallbackUnsupportedDetail) and killed guest execution. This is a
  // fresh executor/context so it does not depend on the shared mutable
  // `state` exercised by the tests above.
  write_instruction(memory, kCode + 0x200u, 0x1C640006u);  // mulli r3, r4, 6
  write_instruction(memory, kCode + 0x204u, 0x7CA321D6u);  // mullw r5, r3, r4
  write_instruction(memory, kCode + 0x208u, 0x7CC52396u);  // divwu r6, r5, r4
  write_instruction(memory, kCode + 0x20Cu, 0x4E800020u);  // blr
  DynamicFallbackExecutor muldiv_fallback;
  ExecutionContext muldiv_context(state, memory, runtime);
  muldiv_fallback.bind(muldiv_context);
  state = {};
  state.gpr[4] = 7u;
  state.lr = kReturn;
  const auto muldiv =
      muldiv_context.try_dynamic_fallback(kCode + 0x200u, CompiledLookupKind::Call);
  assert(muldiv.handled);
  assert(muldiv.result.reason == FlowReason::Return);
  assert(muldiv.result.next_address == kReturn);
  assert(state.gpr[3] == 42u);   // mulli:  4 * 6
  assert(state.gpr[5] == 294u);  // mullw:  42 * 7
  assert(state.gpr[6] == 42u);   // divwu: 294 / 7

  // Real AC6 repro, part two: past the mulli fix, the very next unsupported
  // instruction dynamic fallback hit was "lfs" - this interpreter had no
  // floating-point support at all (no loads/stores, no arithmetic). Exercise
  // a load (single->double promotion), a single-precision add (double->
  // single->double round-trip), a double-precision multiply, and a store,
  // matching real compiled-code shape (e.g. `float x = a + a; something(x*x);`).
  constexpr GuestAddress kFloatSrc = kDataPage + 0x000u;
  constexpr GuestAddress kDoubleDst = kDataPage + 0x100u;
  memory.write32_be(kFloatSrc, std::bit_cast<std::uint32_t>(2.5f));
  write_instruction(memory, kCode + 0x500u, 0xC0230000u);  // lfs f1, 0(r3)
  write_instruction(memory, kCode + 0x504u, 0xEC41082Au);  // fadds f2, f1, f1
  write_instruction(memory, kCode + 0x508u, 0xFC6200B2u);  // fmul f3, f2, f2
  write_instruction(memory, kCode + 0x50Cu, 0xD8640008u);  // stfd f3, 8(r4)
  write_instruction(memory, kCode + 0x510u, 0x4E800020u);  // blr
  DynamicFallbackExecutor fpu_fallback;
  ExecutionContext fpu_context(state, memory, runtime);
  fpu_fallback.bind(fpu_context);
  state = {};
  state.gpr[3] = kFloatSrc;
  state.gpr[4] = kDoubleDst - 8u;
  state.lr = kReturn;
  const auto fpu_result =
      fpu_context.try_dynamic_fallback(kCode + 0x500u, CompiledLookupKind::Call);
  assert(fpu_result.handled);
  assert(fpu_result.result.reason == FlowReason::Return);
  assert(fpu_result.result.next_address == kReturn);
  assert(std::bit_cast<double>(state.fpr_bits[1]) == 2.5);   // lfs promoted to double
  assert(std::bit_cast<double>(state.fpr_bits[2]) == 5.0);   // fadds: 2.5 + 2.5
  assert(std::bit_cast<double>(state.fpr_bits[3]) == 25.0);  // fmul: 5.0 * 5.0
  assert(std::bit_cast<double>(memory.read64_be(kDoubleDst)) == 25.0);  // stfd

  // Real AC6 repro, part three: past the FPU fix, the next unsupported
  // instruction was "addic." (this codebase's internal "addicx" name) - CA
  // (carry) semantics that neither "addi"/"addis" nor any other case here
  // modeled.
  write_instruction(memory, kCode + 0x700u, 0x30A40005u);  // addic  r5, r4, 5
  write_instruction(memory, kCode + 0x704u, 0x34C4FFFFu);  // addic. r6, r4, -1
  write_instruction(memory, kCode + 0x708u, 0x4E800020u);  // blr
  DynamicFallbackExecutor addic_fallback;
  ExecutionContext addic_context(state, memory, runtime);
  addic_fallback.bind(addic_context);
  state = {};
  state.gpr[4] = 7u;
  state.lr = kReturn;
  const auto addic_result =
      addic_context.try_dynamic_fallback(kCode + 0x700u, CompiledLookupKind::Call);
  assert(addic_result.handled);
  assert(addic_result.result.reason == FlowReason::Return);
  assert(addic_result.result.next_address == kReturn);
  assert(state.gpr[5] == 12u);        // addic:  7 + 5, no carry
  assert(state.gpr[6] == 6u);         // addic.: 7 + (-1)
  assert(state.xer_ca());            // 6 <u 7 -> carry set
  assert((state.cr >> 28) == 0b0100u);  // CR0 = GT (6 is positive, nonzero)

  // Real AC6 repro, part four: the entire VMX/VMX128 vector unit - the
  // largest single gap this interpreter has ever had - had no case here at
  // all, starting with "lvx128" (AC6's own compiled code executes this
  // constantly for matrix/skinning math). Exercises the classic 32-register
  // encodings (lvx/vaddfp/stvx) - the VX128-extended forms reuse pre-existing,
  // already-relied-upon DecodedInstruction accessors (vx128_vd() etc.) this
  // interpreter does not implement itself, so this proves the new dispatch
  // logic (register indexing, aot::execute_vector wiring, load/store paths)
  // rather than re-verifying the decoder's own bit-field extraction.
  constexpr GuestAddress kVecA = kDataPage + 0x200u;
  constexpr GuestAddress kVecB = kDataPage + 0x210u;
  constexpr GuestAddress kVecSum = kDataPage + 0x220u;
  const float vec_a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  const float vec_b[4] = {5.0f, 6.0f, 7.0f, 8.0f};
  for (unsigned i = 0; i < 4; ++i) {
    memory.write32_be(kVecA + i * 4u, std::bit_cast<std::uint32_t>(vec_a[i]));
    memory.write32_be(kVecB + i * 4u, std::bit_cast<std::uint32_t>(vec_b[i]));
  }
  write_instruction(memory, kCode + 0x800u, 0x7C2018CEu);  // lvx v1, 0, r3  (r3 = kVecA)
  write_instruction(memory, kCode + 0x804u, 0x7C4020CEu);  // lvx v2, 0, r4  (r4 = kVecB)
  write_instruction(memory, kCode + 0x808u, 0x1061100Au);  // vaddfp v3, v1, v2
  write_instruction(memory, kCode + 0x80Cu, 0x7C6029CEu);  // stvx v3, 0, r5 (r5 = kVecSum)
  write_instruction(memory, kCode + 0x810u, 0x4E800020u);  // blr
  DynamicFallbackExecutor vector_fallback;
  ExecutionContext vector_context(state, memory, runtime);
  vector_fallback.bind(vector_context);
  state = {};
  state.gpr[3] = kVecA;
  state.gpr[4] = kVecB;
  state.gpr[5] = kVecSum;
  state.lr = kReturn;
  const auto vector_result =
      vector_context.try_dynamic_fallback(kCode + 0x800u, CompiledLookupKind::Call);
  assert(vector_result.handled);
  assert(vector_result.result.reason == FlowReason::Return);
  assert(vector_result.result.next_address == kReturn);
  for (unsigned i = 0; i < 4; ++i) {
    const auto sum = std::bit_cast<float>(memory.read32_be(kVecSum + i * 4u));
    assert(sum == vec_a[i] + vec_b[i]);
  }

  std::cout << "dynamic fallback tests passed\n";
  return 0;
}
