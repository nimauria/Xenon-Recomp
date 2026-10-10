// xboxkrnl KeTlsAlloc/KeTlsFree/KeTlsGetValue/KeTlsSetValue (ordinals
// 0x152-0x155 / 338-341). Drives each export through
// core::ExportRegistry::invoke() exactly as a guest thunk would.
//
// Real-world context: KeTlsAlloc is the next real export AC6's boot path
// calls once XexCheckExecutablePrivilege was fixed (real guest address
// 0x8238378c) - a correctly-recognized-but-previously-unimplemented import,
// per src/core/session/execution/runtime_services.cpp's Trap path (STATUS_PROCEDURE_NOT_FOUND).

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xboxkrnl_tls_exports.hpp"

using namespace xenon;

namespace {

constexpr std::uint32_t kOrdAlloc = 0x152u;
constexpr std::uint32_t kOrdFree = 0x153u;
constexpr std::uint32_t kOrdGetValue = 0x154u;
constexpr std::uint32_t kOrdSetValue = 0x155u;
constexpr std::uint32_t kTlsOutOfIndexes = 0xFFFFFFFFu;

struct Fixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  std::shared_ptr<kernel::KernelMemory> kernel_memory;
  std::unique_ptr<kernel::KernelProcess> process;
  std::shared_ptr<kernel::KernelThread> thread;
  core::ExportRegistry registry;

  Fixture() {
    address_space = std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    assert(address_space->initialize());
    kernel_memory = std::make_shared<kernel::KernelMemory>(address_space);
    process = std::make_unique<kernel::KernelProcess>(kernel_memory);
    thread = process->thread_manager().create_thread([] { return 0u; }, {});
    assert(thread != nullptr);
    assert(xbox::register_xboxkrnl_tls_exports(registry, *process));
  }

  [[nodiscard]] core::ExportCallResult invoke(std::uint32_t ordinal, cpu::CpuState& cpu) {
    core::ExportCallContext call{cpu, *address_space, 0, thread->thread_id()};
    return registry.invoke("xboxkrnl", ordinal, call);
  }
};

void test_ordinals_are_registered() {
  Fixture fixture;
  assert(fixture.registry.contains("xboxkrnl.exe", kOrdAlloc));
  assert(fixture.registry.contains("xboxkrnl", "KeTlsAlloc"));
  assert(fixture.registry.contains("xboxkrnl", kOrdFree));
  assert(fixture.registry.contains("xboxkrnl", "KeTlsFree"));
  assert(fixture.registry.contains("xboxkrnl", kOrdGetValue));
  assert(fixture.registry.contains("xboxkrnl", "KeTlsGetValue"));
  assert(fixture.registry.contains("xboxkrnl", kOrdSetValue));
  assert(fixture.registry.contains("xboxkrnl", "KeTlsSetValue"));
}

// A freshly allocated slot must read back as 0 for the allocating thread
// (xenia's KeTlsAlloc explicitly resets the calling thread's value).
void test_alloc_resets_calling_thread_value() {
  Fixture fixture;
  cpu::CpuState alloc_cpu{};
  assert(fixture.invoke(kOrdAlloc, alloc_cpu).success);
  const auto slot = static_cast<std::uint32_t>(alloc_cpu.gpr[3]);
  assert(slot != kTlsOutOfIndexes);

  cpu::CpuState get_cpu{};
  get_cpu.gpr[3] = slot;
  assert(fixture.invoke(kOrdGetValue, get_cpu).success);
  assert(get_cpu.gpr[3] == 0u);
}

// Set then Get on the same thread must observe the stored value.
void test_set_then_get_round_trips() {
  Fixture fixture;
  cpu::CpuState alloc_cpu{};
  assert(fixture.invoke(kOrdAlloc, alloc_cpu).success);
  const auto slot = static_cast<std::uint32_t>(alloc_cpu.gpr[3]);

  cpu::CpuState set_cpu{};
  set_cpu.gpr[3] = slot;
  set_cpu.gpr[4] = 0xDEADBEEFu;
  assert(fixture.invoke(kOrdSetValue, set_cpu).success);
  assert(set_cpu.gpr[3] == 1u && "KeTlsSetValue must report success");

  cpu::CpuState get_cpu{};
  get_cpu.gpr[3] = slot;
  assert(fixture.invoke(kOrdGetValue, get_cpu).success);
  assert(get_cpu.gpr[3] == 0xDEADBEEFu);
}

// Freeing a slot returns it to the free pool so a later Alloc can reuse it -
// proving allocation state is real, not a monotonically-increasing counter.
void test_free_allows_slot_reuse() {
  Fixture fixture;
  cpu::CpuState alloc_cpu{};
  assert(fixture.invoke(kOrdAlloc, alloc_cpu).success);
  const auto slot = static_cast<std::uint32_t>(alloc_cpu.gpr[3]);

  cpu::CpuState free_cpu{};
  free_cpu.gpr[3] = slot;
  assert(fixture.invoke(kOrdFree, free_cpu).success);
  assert(free_cpu.gpr[3] == 1u);

  // Exhaust every remaining slot; the freed one must be among what comes
  // back out, proving it genuinely returned to the pool.
  bool saw_reused_slot = false;
  for (int i = 0; i < 64; ++i) {
    cpu::CpuState realloc_cpu{};
    assert(fixture.invoke(kOrdAlloc, realloc_cpu).success);
    if (static_cast<std::uint32_t>(realloc_cpu.gpr[3]) == slot) saw_reused_slot = true;
  }
  assert(saw_reused_slot);
}

// KeTlsFree(kTlsOutOfIndexes) must fail cleanly (the real xboxkrnl's only
// documented error branch), not corrupt the allocator.
void test_free_out_of_indexes_fails() {
  Fixture fixture;
  cpu::CpuState free_cpu{};
  free_cpu.gpr[3] = kTlsOutOfIndexes;
  assert(fixture.invoke(kOrdFree, free_cpu).success);
  assert(free_cpu.gpr[3] == 0u);
}

// GetValue on a never-allocated slot must return 0, never fault - the real
// xboxkrnl has no error branch here at all.
void test_get_value_on_unallocated_slot_returns_zero() {
  Fixture fixture;
  cpu::CpuState get_cpu{};
  get_cpu.gpr[3] = 5u;
  assert(fixture.invoke(kOrdGetValue, get_cpu).success);
  assert(get_cpu.gpr[3] == 0u);
}

// Every slot exhausted must report kTlsOutOfIndexes, not silently wrap or
// corrupt an already-allocated slot.
void test_allocator_reports_out_of_indexes_when_exhausted() {
  Fixture fixture;
  for (int i = 0; i < 64; ++i) {
    cpu::CpuState alloc_cpu{};
    assert(fixture.invoke(kOrdAlloc, alloc_cpu).success);
    assert(static_cast<std::uint32_t>(alloc_cpu.gpr[3]) != kTlsOutOfIndexes);
  }
  cpu::CpuState exhausted_cpu{};
  assert(fixture.invoke(kOrdAlloc, exhausted_cpu).success);
  assert(static_cast<std::uint32_t>(exhausted_cpu.gpr[3]) == kTlsOutOfIndexes);
}

}  // namespace

int main() {
  std::cout << "Testing xboxkrnl KeTls* exports...\n";

  test_ordinals_are_registered();
  test_alloc_resets_calling_thread_value();
  test_set_then_get_round_trips();
  test_free_allows_slot_reuse();
  test_free_out_of_indexes_fails();
  test_get_value_on_unallocated_slot_returns_zero();
  test_allocator_reports_out_of_indexes_when_exhausted();

  std::cout << "All xboxkrnl KeTls* export tests passed!\n";
  return 0;
}
