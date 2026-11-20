// Runs codegen/codegen_register_helpers.cpp's function: a guest prologue that
// calls __savegprlr_28 and an epilogue that tail-branches to __restgprlr_28,
// both bound directly to Xenon's native helpers. The call returns into the
// function, the tail branch returns to the function's caller with the saved
// registers and LR back, and no registry lookup is involved.

#include <cassert>
#include <cstdint>

#include "xenon/cpu/aot_semantics.hpp"
#include "xenon/cpu/flat_memory.hpp"
#include "xenon/recomp/runtime_helpers.hpp"

using namespace xenon::cpu;

ExecutionResult helper_frame_v2(ExecutionContext&);
extern const bool kDirectSave;
extern const bool kDirectRestore;
extern const bool kAnyLookup;

namespace {

class NullRuntime final : public RuntimeServices {
 public:
  ExecutionResult call(GuestAddress, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 0}; }
  ExecutionResult syscall(std::uint32_t, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 0}; }
  ExecutionResult trap(std::uint32_t, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 0}; }
  std::uint64_t read_spr(std::uint32_t, const CpuState&) override { return 0; }
  void write_spr(std::uint32_t, std::uint64_t, CpuState&) override {}
  std::uint64_t read_time_base(const CpuState&) override { return 0; }
};

}  // namespace

int main() {
  assert(kDirectSave && "the call to the save helper must be direct");
  assert(kDirectRestore && "the tail branch to the restore helper must be a direct tail call");
  assert(!kAnyLookup && "neither helper may go through the registry lookup");

  namespace helpers = xenon::recomp::runtime_helpers;
  FlatMemory memory(0x1000u, 0x1000u);
  NullRuntime runtime;
  CpuState state{};
  constexpr std::uint64_t kStack = 0x1800u;
  state.gpr[1] = kStack;
  for (unsigned reg = 28; reg <= 31; ++reg) state.gpr[reg] = 0x1111'0000'0000'0000ull * (reg - 27u) + reg;
  state.lr = 0x9000u;  // the function's caller
  ExecutionContext context(state, memory, runtime);
  const auto result = helper_frame_v2(context);

  // Returned to the caller through the LR the save helper stashed.
  assert(result.reason == FlowReason::Return && result.next_address == 0x9000u);
  assert(state.lr == 0x9000u);
  for (unsigned reg = 28; reg <= 31; ++reg) {
    const auto expected = 0x1111'0000'0000'0000ull * (reg - 27u) + reg;
    assert(state.gpr[reg] == expected && "the restore helper reloads what the save stored");
    const auto slot = static_cast<GuestAddress>(
        kStack + helpers::kGprLrBaseOffset + static_cast<std::int32_t>((reg - 14u) * 8u));
    assert(memory.read64_be(slot) == expected);
  }
  assert(memory.read32_be(static_cast<GuestAddress>(kStack + helpers::kGprLrLrOffset)) == 0x9000u);
  return 0;
}
