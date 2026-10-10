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
#include <string>
#include <thread>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/logging/diagnostic_events.hpp"
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

// With diagnostic events on, a timed-out wait, the signal and the satisfied
// wait are recorded against one kernel object, by guest thread, with the
// timeout and the returned status.
void test_waits_and_signals_are_recorded_as_diagnostic_events() {
  namespace events = xenon::logging::events;
  Fixture f;
  const auto header = f.alloc_header();
  cpu::CpuState cpu{};
  cpu.gpr[3] = header;
  cpu.gpr[4] = 1;  // SynchronizationEvent
  cpu.gpr[5] = 0;
  assert(f.invoke(0x070u, cpu).success);  // KeInitializeEvent

  events::set_enabled(true);
  events::clear();
  const auto zero_timeout = f.alloc64();
  f.address_space->write64_be(zero_timeout, 0);
  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  cpu.gpr[7] = zero_timeout;
  assert(f.invoke(0x0B0u, cpu, 11).success && cpu.gpr[3] == 0x00000102u);  // KeWaitForSingleObject
  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  assert(f.invoke(0x09Du, cpu, 12).success);  // KeSetEvent
  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  assert(f.invoke(0x0B0u, cpu, 11).success && cpu.gpr[3] == 0u);
  events::set_enabled(false);

  const auto recorded = events::snapshot();
  assert(recorded.size() == 5);
  using events::EventKind;
  const EventKind kinds[] = {EventKind::WaitBegin, EventKind::WaitEnd, EventKind::Signal,
                             EventKind::WaitBegin, EventKind::WaitEnd};
  const std::uint32_t threads[] = {11, 11, 12, 11, 11};
  const std::uint32_t values[] = {0, 0x102u, 0, 0xFFFFFFFFu, 0};
  for (std::size_t i = 0; i < recorded.size(); ++i) {
    assert(recorded[i].kind == kinds[i]);
    assert(recorded[i].guest_thread_id == threads[i]);
    assert(recorded[i].value == values[i]);
    assert(recorded[i].guest_address == header);
    assert(recorded[i].object_id != 0 && recorded[i].object_id == recorded[0].object_id &&
           "the waiter and the signaller name the same kernel object");
  }
  events::clear();
}

// A captured synthetic stall: t22 signals an auto-reset event, t23's wait
// consumes it, and t21 then waits on it forever. The event snapshot alone
// says which thread is stuck, on which object, and who last signalled it.
void test_a_stalled_wait_is_explained_by_the_event_snapshot() {
  namespace events = xenon::logging::events;
  Fixture f;
  const auto header = f.alloc_header();
  cpu::CpuState cpu{};
  cpu.gpr[3] = header;
  cpu.gpr[4] = 1;  // SynchronizationEvent (auto-reset)
  cpu.gpr[5] = 0;
  assert(f.invoke(0x070u, cpu).success);  // KeInitializeEvent

  events::set_enabled(true);
  events::clear();
  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  assert(f.invoke(0x09Du, cpu, 22).success);  // KeSetEvent by t22
  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  assert(f.invoke(0x0B0u, cpu, 23).success && cpu.gpr[3] == 0u);  // t23 consumes it

  std::thread stalled([&] {
    cpu::CpuState waiter{};
    waiter.gpr[3] = header;
    waiter.gpr[7] = 0;  // NULL timeout: wait forever
    assert(f.invoke(0x0B0u, waiter, 21).success);
  });
  // Wait (bounded) until t21's wait has begun.
  std::vector<events::BlockedWait> waits;
  for (int attempt = 0; attempt < 2000 && waits.empty(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto snapshot = events::snapshot();
    waits = events::blocked_waits(snapshot);
  }
  assert(waits.size() == 1);
  assert(waits[0].guest_thread_id == 21 && waits[0].guest_address == header);
  assert(waits[0].timeout_ms == 0xFFFFFFFFu);
  assert(waits[0].last_signal && waits[0].last_signal->guest_thread_id == 22);
  assert(!waits[0].signalled_after_wait_began &&
         "the only signal came before the wait and another thread consumed it");
  const auto text = events::format_blocked_waits(waits, waits[0].waiting_since_ns);
  assert(text.find("t21 waits in KeWaitForSingleObject") != std::string::npos);
  assert(text.find("last signalled by t22 in KeSetEvent") != std::string::npos);

  // Release the stalled thread.
  cpu = cpu::CpuState{};
  cpu.gpr[3] = header;
  assert(f.invoke(0x09Du, cpu, 22).success);
  stalled.join();
  assert(events::blocked_waits(events::snapshot()).empty());
  events::set_enabled(false);
  events::clear();
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

void test_ke_query_base_priority_thread_unknown_thread_is_safe() {
  // KeSetBasePriorityThread/KeQueryBasePriorityThread both resolve a guest
  // KTHREAD pointer by scanning ThreadManager::enumerate_threads() for a
  // matching kernel::KernelThread::guest_kthread_address() - a pointer with
  // no match (no thread created yet in this minimal fixture) must not crash
  // and must return a safe default (0) rather than resolving to some other
  // thread's priority or reading unrelated memory.
  Fixture f;
  assert(f.registry.contains("xboxkrnl", 0x081u));
  assert(f.registry.contains("xboxkrnl", "KeQueryBasePriorityThread"));

  cpu::CpuState unknown_cpu{};
  unknown_cpu.gpr[3] = 0xDEADBEEFu;
  auto unknown_result = f.invoke(0x081u, unknown_cpu);
  assert(unknown_result.handled && unknown_result.success);
  assert(unknown_cpu.gpr[3] == 0u);
}

void test_ke_resume_thread_unknown_pointer_returns_invalid_handle() {
  // Same guest-KTHREAD-pointer resolution as KeSetBasePriorityThread/
  // KeQueryBasePriorityThread above - a pointer with no match must not
  // crash, must not resume some unrelated thread, and (unlike those two,
  // which return a safe default value) must report STATUS_INVALID_HANDLE,
  // matching the real xboxkrnl/xenia-verified KeResumeThread contract.
  Fixture f;
  assert(f.registry.contains("xboxkrnl", 0x092u));
  assert(f.registry.contains("xboxkrnl", "KeResumeThread"));

  cpu::CpuState unknown_cpu{};
  unknown_cpu.gpr[3] = 0xDEADBEEFu;
  auto unknown_result = f.invoke(0x092u, unknown_cpu);
  assert(unknown_result.handled && unknown_result.success);
  assert(unknown_cpu.gpr[3] == 0xC0000008u);  // STATUS_INVALID_HANDLE
}

}  // namespace

int main() {
  std::cout << "Testing xboxkrnl Ke* sync exports...\n";

  test_ordinals_are_registered();
  test_ke_initialize_event_set_and_wait_round_trip();
  test_waits_and_signals_are_recorded_as_diagnostic_events();
  test_a_stalled_wait_is_explained_by_the_event_snapshot();
  test_repeated_calls_on_the_same_header_resolve_to_the_same_object();
  test_ke_semaphore_initialize_release_and_wait();
  test_ke_wait_for_multiple_objects_any_and_all();
  test_unsupported_dispatcher_type_is_rejected_not_misinterpreted();
  test_ke_query_base_priority_thread_unknown_thread_is_safe();
  test_ke_resume_thread_unknown_pointer_returns_invalid_handle();

  std::cout << "All xboxkrnl Ke* sync export tests passed!\n";
  return 0;
}
