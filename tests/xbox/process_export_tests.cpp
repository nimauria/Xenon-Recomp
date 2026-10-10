// xboxkrnl process-type exports (KeGetCurrentProcessType,
// KeSetCurrentProcessType). Drives each export through
// core::ExportRegistry::invoke() exactly as a guest thunk would.
//
// Real-world context: KeGetCurrentProcessType (ordinal 102 / 0x66) is the
// next real export AC6's boot path calls once KeFlushUserModeTb,
// RtlImageXexHeaderField and NtAllocateVirtualMemory were all fixed (real
// guest address 0x823d03fc) - a correctly-recognized-but-previously-
// unimplemented import, per src/core/session/execution/runtime_services.cpp's Trap path
// (STATUS_PROCEDURE_NOT_FOUND).

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xboxkrnl_process_exports.hpp"

using namespace xenon;

namespace {

constexpr std::uint32_t kProcTypeIdle = 0u;
constexpr std::uint32_t kProcTypeUser = 1u;
constexpr std::uint32_t kProcTypeSystem = 2u;

struct Fixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  std::shared_ptr<kernel::KernelMemory> kernel_memory;
  std::unique_ptr<kernel::KernelProcess> process;
  core::ExportRegistry registry;

  Fixture() {
    address_space = std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    assert(address_space->initialize());
    kernel_memory = std::make_shared<kernel::KernelMemory>(address_space);
    process = std::make_unique<kernel::KernelProcess>(kernel_memory);
    assert(xbox::register_xboxkrnl_process_exports(registry, *process));
  }

  [[nodiscard]] core::ExportCallResult invoke(std::uint32_t ordinal, cpu::CpuState& cpu) {
    core::ExportCallContext call{cpu, *address_space, 0, 0};
    return registry.invoke("xboxkrnl", ordinal, call);
  }
};

void test_ordinals_are_registered() {
  Fixture fixture;
  assert(fixture.registry.contains("xboxkrnl.exe", 0x066u));
  assert(fixture.registry.contains("xboxkrnl", "KeGetCurrentProcessType"));
  assert(fixture.registry.contains("xboxkrnl", 0x09Au));
  assert(fixture.registry.contains("xboxkrnl", "KeSetCurrentProcessType"));

  // Registering twice must remain safe (session re-init calls this again).
  assert(xbox::register_xboxkrnl_process_exports(fixture.registry, *fixture.process));
}

// The real, verified Xbox 360 default for a running title process is
// X_PROCTYPE_USER (1) - not an arbitrary/undefined value.
void test_default_process_type_is_user() {
  Fixture fixture;
  cpu::CpuState cpu{};
  const auto result = fixture.invoke(0x066u, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == kProcTypeUser);
}

// A title that calls Set then Get must observe its own change - real,
// mutable state, not a hardcoded constant.
void test_set_then_get_round_trips() {
  Fixture fixture;

  cpu::CpuState set_cpu{};
  set_cpu.gpr[3] = kProcTypeSystem;
  assert(fixture.invoke(0x09Au, set_cpu).success);

  cpu::CpuState get_cpu{};
  const auto result = fixture.invoke(0x066u, get_cpu);
  assert(result.handled && result.success);
  assert(get_cpu.gpr[3] == kProcTypeSystem);

  cpu::CpuState set_idle_cpu{};
  set_idle_cpu.gpr[3] = kProcTypeIdle;
  assert(fixture.invoke(0x09Au, set_idle_cpu).success);
  cpu::CpuState get_idle_cpu{};
  assert(fixture.invoke(0x066u, get_idle_cpu).success);
  assert(get_idle_cpu.gpr[3] == kProcTypeIdle);
}

}  // namespace

int main() {
  std::cout << "Testing xboxkrnl process-type exports...\n";

  test_ordinals_are_registered();
  test_default_process_type_is_user();
  test_set_then_get_round_trips();

  std::cout << "All xboxkrnl process-type export tests passed!\n";
  return 0;
}
