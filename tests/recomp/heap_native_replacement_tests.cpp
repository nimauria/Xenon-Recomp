#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>

#include "xenon/cpu/runtime.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/types.hpp"
#include "xenon/recomp/analysis_schema.hpp"
#include "xenon/recomp/native_replacements.hpp"

using namespace xenon::cpu;
namespace analysis = xenon::recomp::analysis;
namespace replacements = xenon::recomp::native_replacements;

namespace {
class TestRuntime final : public RuntimeServices {
 public:
  explicit TestRuntime(xenon::kernel::KernelProcess& process) : process_(process) {}
  ExecutionResult call(GuestAddress target, CpuState&, MemoryPort&) override {
    return {FlowReason::Branch, target, 0};
  }
  ExecutionResult syscall(std::uint32_t level, CpuState& state, MemoryPort&) override {
    return {FlowReason::Syscall, state.cia + 4u, level};
  }
  ExecutionResult trap(std::uint32_t code, CpuState& state, MemoryPort&) override {
    return {FlowReason::Trap, state.cia, code};
  }
  std::uint64_t read_spr(std::uint32_t, const CpuState&) override { return 0; }
  void write_spr(std::uint32_t, std::uint64_t, CpuState&) override {}
  std::uint64_t read_time_base(const CpuState& state) override { return state.time_base; }
  xenon::kernel::KernelProcess* current_process() noexcept override { return &process_; }
 private:
  xenon::kernel::KernelProcess& process_;
};
}  // namespace

int main() {
  auto address_space = std::make_shared<xenon::memory::AddressSpace>(
      xenon::memory::GuestTranslationMode::Compact);
  assert(address_space->initialize());
  auto memory = std::make_shared<xenon::kernel::KernelMemory>(address_space);
  xenon::kernel::KernelProcess process(memory);
  TestRuntime runtime(process);
  CpuState state{};
  ExecutionContext context(state, *address_space, runtime);

  const auto alloc = replacements::entry_for(analysis::NativeReplacementKind::HeapAllocate);
  const auto free = replacements::entry_for(analysis::NativeReplacementKind::HeapFree);
  const auto size = replacements::entry_for(analysis::NativeReplacementKind::HeapSize);
  const auto realloc = replacements::entry_for(analysis::NativeReplacementKind::HeapReAllocate);
  assert(alloc && free && size && realloc);

  constexpr std::uint32_t kHeap = 0xAABBCCDDu;
  state.gpr[3] = kHeap;
  state.gpr[4] = xenon::kernel::GuestHeapManager::kHeapZeroMemory;
  state.gpr[5] = 64u;
  assert(alloc(context).reason == FlowReason::Return);
  const auto pointer = static_cast<std::uint32_t>(state.gpr[3]);
  assert(pointer != 0u);
  for (std::uint32_t i = 0; i < 64u; ++i) assert(address_space->read8(pointer + i) == 0u);
  address_space->write32_be(pointer, 0x11223344u);

  state.gpr[3] = kHeap;
  state.gpr[4] = 0u;
  state.gpr[5] = pointer;
  assert(size(context).reason == FlowReason::Return);
  assert(state.gpr[3] == 64u);

  state.gpr[3] = kHeap;
  state.gpr[4] = 0u;
  state.gpr[5] = pointer;
  state.gpr[6] = 0x20000u;
  assert(realloc(context).reason == FlowReason::Return);
  const auto grown = static_cast<std::uint32_t>(state.gpr[3]);
  assert(grown != 0u && address_space->read32_be(grown) == 0x11223344u);

  state.gpr[3] = kHeap;
  state.gpr[4] = 0u;
  state.gpr[5] = grown;
  assert(free(context).reason == FlowReason::Return);
  assert(state.gpr[3] == 1u);

  state.gpr[3] = kHeap;
  state.gpr[4] = 0u;
  state.gpr[5] = grown;
  assert(free(context).reason == FlowReason::Return);
  assert(state.gpr[3] == 0u && "double free must be rejected");

  process.terminate(0);
  std::cout << "Heap native replacement tests passed\n";
  return 0;
}
