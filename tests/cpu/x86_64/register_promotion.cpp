// Runs codegen/codegen_register_promotion.cpp's scenarios with guest
// registers in CpuState and promoted to locals, from identical state and
// memory: registers, memory, results and what a trap handler saw must match,
// and the promoted results must be the right ones.

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#include "xenon/cpu/aot_semantics.hpp"
#include "xenon/cpu/flat_memory.hpp"

using namespace xenon::cpu;

#define XENON_SCENARIO(name)                    \
  ExecutionResult name##_plain_v2(ExecutionContext&); \
  ExecutionResult name##_promoted_v2(ExecutionContext&);
XENON_SCENARIO(loop)
XENON_SCENARIO(fp)
XENON_SCENARIO(vector)
XENON_SCENARIO(string_load)
XENON_SCENARIO(trap)
XENON_SCENARIO(caller)
#undef XENON_SCENARIO
extern const bool kPromotedFunctionsPromote;
extern const bool kPlainFunctionsPromote;

namespace {

constexpr GuestAddress kBase = 0x1000u;
constexpr std::uint64_t kData = 0x1100u;
constexpr std::uint64_t kOut = 0x1200u;

class RecordingRuntime final : public RuntimeServices {
 public:
  ExecutionResult call(GuestAddress, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 1}; }
  ExecutionResult syscall(std::uint32_t, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 2}; }
  ExecutionResult trap(std::uint32_t, CpuState& state, MemoryPort&) override {
    trapped_r3 = state.gpr[3];
    return {FlowReason::Trap, static_cast<GuestAddress>(state.cia), 3};
  }
  std::uint64_t read_spr(std::uint32_t, const CpuState&) override { return 0; }
  void write_spr(std::uint32_t, std::uint64_t, CpuState&) override {}
  std::uint64_t read_time_base(const CpuState&) override { return 0; }
  std::uint64_t trapped_r3{~0ull};
};

struct Outcome {
  ExecutionResult result{};
  CpuState state{};
  std::vector<std::uint8_t> memory;
  std::uint64_t trapped_r3{};
};

Outcome run(ExecutionResult (*fn)(ExecutionContext&)) {
  FlatMemory memory(0x1000u, kBase);
  // Inputs: a double, four floats and eight bytes at kData.
  memory.write64_be(kData, std::bit_cast<std::uint64_t>(1.5));
  for (std::uint32_t i = 0; i < 4u; ++i)
    memory.write32_be(static_cast<GuestAddress>(kData + 16u + i * 4u), std::bit_cast<std::uint32_t>(0.25f + i));
  for (std::uint32_t i = 0; i < 8u; ++i) memory.write8(static_cast<GuestAddress>(kData + 32u + i), static_cast<std::uint8_t>(0x10u + i));
  RecordingRuntime runtime;
  Outcome outcome;
  auto& state = outcome.state;
  state.gpr[1] = 0x1F00u;
  state.gpr[5] = kData;
  state.gpr[6] = kOut;
  state.gpr[8] = kData + 32u;
  state.lr = 0x9000u;
  ExecutionContext context(state, memory, runtime);
  outcome.result = fn(context);
  outcome.trapped_r3 = runtime.trapped_r3;
  for (std::uint32_t i = 0; i < 0x1000u; ++i) outcome.memory.push_back(memory.read8(kBase + i));
  return outcome;
}

void same(const Outcome& a, const Outcome& b) {
  assert(a.result.reason == b.result.reason && a.result.next_address == b.result.next_address &&
         a.result.detail == b.result.detail);
  assert(a.state.gpr == b.state.gpr);
  assert(a.state.fpr_bits == b.state.fpr_bits);
  for (std::size_t i = 0; i < a.state.vr.size(); ++i) assert(a.state.vr[i].bytes == b.state.vr[i].bytes);
  assert(a.state.lr == b.state.lr && a.state.cr == b.state.cr && a.state.cia == b.state.cia);
  assert(a.memory == b.memory);
  assert(a.trapped_r3 == b.trapped_r3);
}

Outcome both(ExecutionResult (*plain)(ExecutionContext&), ExecutionResult (*promoted)(ExecutionContext&)) {
  const auto a = run(plain);
  const auto b = run(promoted);
  same(a, b);
  return b;
}

}  // namespace

int main() {
  assert(kPromotedFunctionsPromote && "the promoted functions must hold registers in locals");
  assert(!kPlainFunctionsPromote);

  {
    const auto r = both(&loop_plain_v2, &loop_promoted_v2);
    assert(r.state.gpr[3] == 200u && r.state.gpr[4] == 0u);
    assert(r.memory[kData - kBase + 3u] == 200u);  // stw r3,0(r5), big-endian low byte
  }
  {
    const auto r = both(&fp_plain_v2, &fp_promoted_v2);
    assert(std::bit_cast<double>(r.state.fpr_bits[2]) == 3.0);
  }
  {
    const auto r = both(&vector_plain_v2, &vector_promoted_v2);
    (void)r;
  }
  {
    const auto r = both(&string_load_plain_v2, &string_load_promoted_v2);
    // lswi r3,r8,8 loaded the 8 bytes at r8 into r3 and r4 (32 bits each).
    assert(r.state.gpr[3] == 0x10111213u && r.state.gpr[4] == 0x14151617u);
    assert(r.state.gpr[6] == 0x10111213u + 0x14151617u);
  }
  {
    const auto r = both(&trap_plain_v2, &trap_promoted_v2);
    assert(r.result.reason == FlowReason::Trap && r.trapped_r3 == 77u &&
           "the trap handler must see the promoted r3");
  }
  {
    const auto r = both(&caller_plain_v2, &caller_promoted_v2);
    assert(r.result.reason == FlowReason::Return && r.result.next_address == 0x9000u);
    assert(r.state.gpr[3] == 36u && r.state.gpr[4] == 9u && r.state.gpr[7] == 45u &&
           "the caller must see what the callee left in r3 and r4");
  }
  return 0;
}
