// Runs the functions codegen/codegen_large_function.cpp generated: a function
// past the backend's size limit (compiled without MSVC global optimization)
// computes exactly what the small one does, and only it carries the pragma.

#include <cassert>
#include <cstdint>

#include "xenon/cpu/aot_semantics.hpp"
#include "xenon/cpu/flat_memory.hpp"

using namespace xenon::cpu;

ExecutionResult large_counting_v2(ExecutionContext&);
ExecutionResult small_counting_v2(ExecutionContext&);
extern const bool kLargeCountingReduced;
extern const bool kSmallCountingReduced;

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

void run(ExecutionResult (*fn)(ExecutionContext&), std::uint64_t units) {
  FlatMemory memory(0x1000u, 0x1000u);
  NullRuntime runtime;
  CpuState state{};
  state.gpr[3] = 7u;
  state.gpr[4] = 100u;
  state.lr = 0x9000u;
  ExecutionContext context(state, memory, runtime);
  const auto result = fn(context);
  assert(result.reason == FlowReason::Return && result.next_address == 0x9000u);
  assert(state.gpr[3] == 7u + units);
  assert(state.gpr[4] == 100u + units);
  // cmpwi r3,-1 last compared a positive value: cr0 = GT.
  assert(state.cr_field(0) == 0x4u);
}

}  // namespace

int main() {
  assert(kLargeCountingReduced);
  assert(!kSmallCountingReduced);
  run(&large_counting_v2, 3000u);
  run(&small_counting_v2, 8u);
  return 0;
}
