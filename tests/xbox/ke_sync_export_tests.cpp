// Phase 3 (AC6 Runtime Readiness pass): xboxkrnl raw-guest-memory-object
// (Ke*) synchronization exports. Drives each export through
// core::ExportRegistry::invoke() exactly as a guest thunk would, exercising
// the real X_DISPATCH_HEADER resolution mechanism (xex_dispatcher_header.hpp)
// against real guest memory - not by calling the handler C++ function or
// resolve_dispatcher_object() directly.

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xboxkrnl_ke_sync_exports.hpp"

using namespace xenon;

namespace {

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
    assert(xbox::register_xboxkrnl_ke_sync_exports(registry, *process));
  }

  [[nodiscard]] core::ExportCallResult invoke(std::uint32_t ordinal, cpu::CpuState& cpu,
                                              std::uint32_t thread_id = 0) {
    core::ExportCallContext call{cpu, *address_space, 0, thread_id};
    return registry.invoke("xboxkrnl", ordinal, call);
  }

  [[nodiscard]] memory::GuestAddress alloc_header() {
    // X_DISPATCH_HEADER is 0x10 bytes; allocate a little extra so an
    // X_KSEMAPHORE's trailing `limit` field (offset 0x10) is always safe to
    // touch too.
    memory::GuestAddress addr{};
    assert(address_space->allocate(0x20, 8, memory::kReadWrite, false, addr));
    return addr;
  }
  [[nodiscard]] memory::GuestAddress alloc64() {
    memory::GuestAddress addr{};
    assert(address_space->allocate(8, 8, memory::kReadWrite, false, addr));
    return addr;
  }
};

void test_ordinals_are_registered() {
  Fixture f;
  assert(f.registry.contains("xboxkrnl", 0x070u));
  assert(f.registry.contains("xboxkrnl", "KeInitializeEvent"));
  assert(f.registry.contains("xboxkrnl", 0x074u));
  assert(f.registry.contains("xboxkrnl", "KeInitializeSemaphore"));
  assert(f.registry.contains("xboxkrnl", 0x08Fu));
  assert(f.registry.contains("xboxkrnl", "KeResetEvent"));
  assert(f.registry.contains("xboxkrnl", 0x088u));
  assert(f.registry.contains("xboxkrnl", "KeReleaseSemaphore"));
  assert(f.registry.contains("xboxkrnl", 0x09Du));
  assert(f.registry.contains("xboxkrnl", "KeSetEvent"));
  assert(f.registry.contains("xboxkrnl", 0x0AFu));
  assert(f.registry.contains("xboxkrnl", "KeWaitForMultipleObjects"));
  assert(f.registry.contains("xboxkrnl", 0x0B0u));
  assert(f.registry.contains("xboxkrnl", "KeWaitForSingleObject"));
}

void test_ke_initialize_event_set_and_wait_round_trip() {
  Fixture f;
  const auto header = f.alloc_header();

  cpu::CpuState cpu{};
  cpu.gpr[3] = header;
  cpu.gpr[4] = 1;  // SynchronizationEvent (auto-reset)
  cpu.gpr[5] = 0;  // initial state: not signaled
  auto result = f.invoke(0x070u, cpu);  // KeInitializeEvent
  assert(result.handled && result.success);

  // Not signaled yet: a zero-timeout wait must time out.
  const auto zero_timeout = f.alloc64();
  f.address_space->write64_be(zero_timeout, 0);
  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  cpu.gpr[7] = zero_timeout;
  result = f.invoke(0x0B0u, cpu);  // KeWaitForSingleObject
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0x00000102u);  // STATUS_TIMEOUT

  // KeSetEvent: previous state must be 0 (not signaled before this call).
  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  result = f.invoke(0x09Du, cpu);  // KeSetEvent
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0u);

  // Now waiting must succeed immediately.
  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  cpu.gpr[7] = 0;  // NULL timeout -> infinite, but already signaled
  result = f.invoke(0x0B0u, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0u);  // STATUS_WAIT_0
}

void test_repeated_calls_on_the_same_header_resolve_to_the_same_object() {
  // The core correctness property of the lazy stash mechanism: KeSetEvent
  // followed by KeResetEvent on the SAME guest address must observe the
  // SAME underlying event, not silently create a fresh one each time (which
  // would make every call after the first appear "never signaled").
  Fixture f;
  const auto header = f.alloc_header();

  cpu::CpuState cpu{};
  cpu.gpr[3] = header;
  cpu.gpr[4] = 0;  // NotificationEvent (manual-reset)
  cpu.gpr[5] = 0;
  assert(f.invoke(0x070u, cpu).success);  // KeInitializeEvent

  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  assert(f.invoke(0x09Du, cpu).success);  // KeSetEvent: 0 -> 1
  assert(cpu.gpr[3] == 0u);

  // A second KeSetEvent call must see the object as already signaled (i.e.
  // resolved to the SAME object, not a fresh unsignaled one).
  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  assert(f.invoke(0x09Du, cpu).success);
  assert(cpu.gpr[3] == 1u && "second KeSetEvent must observe the event as already signaled");

  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  assert(f.invoke(0x08Fu, cpu).success);  // KeResetEvent
  assert(cpu.gpr[3] == 1u && "KeResetEvent must report the previous (signaled) state");
}

void test_ke_semaphore_initialize_release_and_wait() {
  Fixture f;
  const auto header = f.alloc_header();

  cpu::CpuState cpu{};
  cpu.gpr[3] = header;
  cpu.gpr[4] = 0;  // initial count
  cpu.gpr[5] = 2;  // limit
  assert(f.invoke(0x074u, cpu).success);  // KeInitializeSemaphore

  // Not available: zero-timeout wait times out.
  const auto zero_timeout = f.alloc64();
  f.address_space->write64_be(zero_timeout, 0);
  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  cpu.gpr[7] = zero_timeout;
  auto result = f.invoke(0x0B0u, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0x00000102u);

  // Release by 1; previous count must be 0.
  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  cpu.gpr[5] = 1;  // adjustment
  result = f.invoke(0x088u, cpu);  // KeReleaseSemaphore
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0u);

  // Now a wait must succeed.
  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  cpu.gpr[7] = 0;
  result = f.invoke(0x0B0u, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0u);

  // Releasing past the limit must not silently succeed.
  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  cpu.gpr[5] = 5;
  result = f.invoke(0x088u, cpu);
  assert(result.handled && result.success);
  // count is now 0 (released to 1, then the wait above consumed it back to
  // 0) - releasing 5 more would exceed limit=2, so the unchanged current
  // count (0) must be reported, not an invented "success" value.
  assert(cpu.gpr[3] == 0u);
}

void test_ke_wait_for_multiple_objects_any_and_all() {
  Fixture f;
  const auto header1 = f.alloc_header();
  const auto header2 = f.alloc_header();

  cpu::CpuState cpu{};
  cpu.gpr[3] = header1;
  cpu.gpr[4] = 0;
  cpu.gpr[5] = 1;  // pre-signaled
  assert(f.invoke(0x070u, cpu).success);

  cpu = cpu::CpuState{};
  cpu.gpr[3] = header2;
  cpu.gpr[4] = 0;
  cpu.gpr[5] = 0;  // not signaled
  assert(f.invoke(0x070u, cpu).success);

  const auto headers_array = f.alloc64();
  f.address_space->write32_be(headers_array, header1);
  f.address_space->write32_be(headers_array + 4u, header2);
  const auto zero_timeout = f.alloc64();
  f.address_space->write64_be(zero_timeout, 0);

  cpu = cpu::CpuState{};
  cpu.gpr[3] = 2;
  cpu.gpr[4] = headers_array;
  cpu.gpr[5] = 0;  // any
  cpu.gpr[9] = zero_timeout;
  auto result = f.invoke(0x0AFu, cpu);  // KeWaitForMultipleObjects
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0u);  // STATUS_WAIT_0 (header1 already signaled)

  cpu = cpu::CpuState{};
  cpu.gpr[3] = 2;
  cpu.gpr[4] = headers_array;
  cpu.gpr[5] = 1;  // all
  cpu.gpr[9] = zero_timeout;
  result = f.invoke(0x0AFu, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0x00000102u);  // header2 is not signaled -> timeout
}

void test_unsupported_dispatcher_type_is_rejected_not_misinterpreted() {
  // type=2 (Mutant) is intentionally unsupported - a guest that passes a
  // KMUTANT header to KeWaitForSingleObject must get a clear failure, never
  // be silently (mis)treated as an event.
  Fixture f;
  const auto header = f.alloc_header();
  f.address_space->write8(header, 2u);  // type = Mutant

  cpu::CpuState cpu{};
  cpu.gpr[3] = header;
  cpu.gpr[7] = 0;
  auto result = f.invoke(0x0B0u, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0xC000000Du);  // STATUS_INVALID_PARAMETER
}

}  // namespace

int main() {
  std::cout << "Testing xboxkrnl Ke* sync exports...\n";

  test_ordinals_are_registered();
  test_ke_initialize_event_set_and_wait_round_trip();
  test_repeated_calls_on_the_same_header_resolve_to_the_same_object();
  test_ke_semaphore_initialize_release_and_wait();
  test_ke_wait_for_multiple_objects_any_and_all();
  test_unsupported_dispatcher_type_is_rejected_not_misinterpreted();

  std::cout << "All xboxkrnl Ke* sync export tests passed!\n";
  return 0;
}
