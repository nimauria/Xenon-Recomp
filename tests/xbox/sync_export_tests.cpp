// Phase 1/3 (AC6 Runtime Readiness pass): xboxkrnl handle-based (Nt*)
// synchronization exports. Drives each export through
// core::ExportRegistry::invoke() exactly as a guest thunk would (real
// ordinal, real ExportCallContext with a real thread_id), not by calling the
// handler C++ function directly - this is what actually proves the
// end-to-end guest ABI (argument registers, output-pointer writes, status
// codes) is correct, not just the underlying kernel:: objects in isolation.

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/mutant.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/semaphore.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xboxkrnl_sync_exports.hpp"

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
    assert(xbox::register_xboxkrnl_sync_exports(registry, *process));
  }

  [[nodiscard]] core::ExportCallResult invoke(std::uint32_t ordinal, cpu::CpuState& cpu,
                                              std::uint32_t thread_id = 0) {
    core::ExportCallContext call{cpu, *address_space, 0, thread_id};
    return registry.invoke("xboxkrnl", ordinal, call);
  }

  [[nodiscard]] memory::GuestAddress alloc32() {
    memory::GuestAddress addr{};
    assert(address_space->allocate(4, 4, memory::kReadWrite, false, addr));
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
  assert(f.registry.contains("xboxkrnl", 0x0D1u));
  assert(f.registry.contains("xboxkrnl", "NtCreateEvent"));
  assert(f.registry.contains("xboxkrnl", 0x0D5u));
  assert(f.registry.contains("xboxkrnl", "NtCreateSemaphore"));
  assert(f.registry.contains("xboxkrnl", 0x0F3u));
  assert(f.registry.contains("xboxkrnl", "NtReleaseSemaphore"));
  assert(f.registry.contains("xboxkrnl", 0x0D4u));
  assert(f.registry.contains("xboxkrnl", "NtCreateMutant"));
  assert(f.registry.contains("xboxkrnl", 0x0F2u));
  assert(f.registry.contains("xboxkrnl", "NtReleaseMutant"));
  assert(f.registry.contains("xboxkrnl", 0x0FDu));
  assert(f.registry.contains("xboxkrnl", "NtWaitForSingleObjectEx"));
  assert(f.registry.contains("xboxkrnl", 0x0FEu));
  assert(f.registry.contains("xboxkrnl", "NtWaitForMultipleObjectsEx"));
  assert(f.registry.contains("xboxkrnl", 0x0D7u));
  assert(f.registry.contains("xboxkrnl", "NtCreateTimer"));
  assert(f.registry.contains("xboxkrnl", 0x0CDu));
  assert(f.registry.contains("xboxkrnl", "NtCancelTimer"));
  assert(f.registry.contains("xboxkrnl", 0x0FAu));
  assert(f.registry.contains("xboxkrnl", "NtSetTimerEx"));
}

void test_nt_create_event_and_wait_single() {
  Fixture f;
  const auto handle_out = f.alloc32();

  cpu::CpuState cpu{};
  cpu.gpr[3] = handle_out;
  cpu.gpr[4] = 0;  // object attributes (unnamed)
  cpu.gpr[5] = 1;  // EVENT_TYPE::SynchronizationEvent (auto-reset)
  cpu.gpr[6] = 0;  // initial state: not signaled
  auto result = f.invoke(0x0D1u, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0u);  // STATUS_SUCCESS

  const auto handle = f.address_space->read32_be(handle_out);
  assert(handle != 0u);

  // Not signaled: a zero-timeout wait must time out, not block indefinitely.
  cpu = cpu::CpuState{};
  cpu.gpr[3] = handle;
  cpu.gpr[6] = f.alloc64();  // timeout pointer: 0 relative ms
  f.address_space->write64_be(cpu.gpr[6], 0);
  result = f.invoke(0x0FDu, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0x00000102u);  // STATUS_TIMEOUT

  // Signal the event directly through the handle table (KeSetEvent is
  // intentionally out of scope this pass - see xboxkrnl_sync_exports.hpp).
  kernel::HandleView view{};
  assert(f.process->handle_table().lookup(handle, view) == kernel::KernelIoCode::Success);
  static_cast<kernel::KernelEvent&>(*view.object).set();

  cpu = cpu::CpuState{};
  cpu.gpr[3] = handle;
  cpu.gpr[6] = 0;  // NULL timeout pointer -> infinite, but object is already signaled
  result = f.invoke(0x0FDu, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0u);  // STATUS_WAIT_0
}

void test_nt_create_semaphore_release_and_limit() {
  Fixture f;
  const auto handle_out = f.alloc32();

  cpu::CpuState cpu{};
  cpu.gpr[3] = handle_out;
  cpu.gpr[4] = 0;
  cpu.gpr[5] = 0;  // initial count
  cpu.gpr[6] = 2;  // maximum count
  auto result = f.invoke(0x0D5u, cpu);
  assert(result.handled && result.success && cpu.gpr[3] == 0u);
  const auto handle = f.address_space->read32_be(handle_out);

  // Release by 1, previous count out pointer must read back 0.
  const auto previous_out = f.alloc32();
  cpu = cpu::CpuState{};
  cpu.gpr[3] = handle;
  cpu.gpr[4] = 1;
  cpu.gpr[5] = previous_out;
  result = f.invoke(0x0F3u, cpu);
  assert(result.handled && result.success && cpu.gpr[3] == 0u);
  assert(f.address_space->read32_be(previous_out) == 0u);

  // Releasing past the maximum count must fail with a distinct status, not
  // silently succeed.
  cpu = cpu::CpuState{};
  cpu.gpr[3] = handle;
  cpu.gpr[4] = 5;
  cpu.gpr[5] = 0;
  result = f.invoke(0x0F3u, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0xC0000047u);  // STATUS_SEMAPHORE_LIMIT_EXCEEDED

  // count > limit at creation time is rejected up front.
  cpu = cpu::CpuState{};
  cpu.gpr[3] = f.alloc32();
  cpu.gpr[5] = 5;
  cpu.gpr[6] = 2;
  result = f.invoke(0x0D5u, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0xC000000Du);  // STATUS_INVALID_PARAMETER
}

void test_nt_create_mutant_uses_real_thread_id_and_ownership() {
  Fixture f;
  constexpr std::uint32_t kOwnerThread = 42;
  constexpr std::uint32_t kOtherThread = 99;

  const auto handle_out = f.alloc32();
  cpu::CpuState cpu{};
  cpu.gpr[3] = handle_out;
  cpu.gpr[4] = 0;
  cpu.gpr[5] = 1;  // initial owner = true
  auto result = f.invoke(0x0D4u, cpu, kOwnerThread);
  assert(result.handled && result.success && cpu.gpr[3] == 0u);
  const auto handle = f.address_space->read32_be(handle_out);

  // The real creating thread id must be the owner - this is the end-to-end
  // regression for the historical hardcoded owner_thread_id_ = 1 bug: with a
  // real thread_id of 42 flowing through ExportCallContext, the mutant must
  // never appear owned by thread 1 (or by 0/unset).
  kernel::HandleView view{};
  assert(f.process->handle_table().lookup(handle, view) == kernel::KernelIoCode::Success);
  auto& mutant = static_cast<kernel::KernelMutant&>(*view.object);
  assert(mutant.is_owned());
  assert(mutant.owner_thread_id() == kOwnerThread);
  assert(mutant.owner_thread_id() != 1u);

  // A different thread releasing it must fail (STATUS_MUTANT_NOT_OWNED), not
  // silently succeed.
  cpu = cpu::CpuState{};
  cpu.gpr[3] = handle;
  cpu.gpr[4] = 0;
  result = f.invoke(0x0F2u, cpu, kOtherThread);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0xC0000046u);  // STATUS_MUTANT_NOT_OWNED
  assert(mutant.is_owned());  // unchanged

  // The real owner releasing it succeeds.
  cpu = cpu::CpuState{};
  cpu.gpr[3] = handle;
  cpu.gpr[4] = 0;
  result = f.invoke(0x0F2u, cpu, kOwnerThread);
  assert(result.handled && result.success && cpu.gpr[3] == 0u);
  assert(!mutant.is_owned());
}

void test_nt_wait_for_multiple_objects_any_and_all() {
  Fixture f;

  // Two semaphores, one pre-signaled (count=1), one not (count=0).
  const auto handle1_out = f.alloc32();
  cpu::CpuState cpu{};
  cpu.gpr[3] = handle1_out;
  cpu.gpr[5] = 1;
  cpu.gpr[6] = 1;
  assert(f.invoke(0x0D5u, cpu).success && cpu.gpr[3] == 0u);
  const auto handle1 = f.address_space->read32_be(handle1_out);

  const auto handle2_out = f.alloc32();
  cpu = cpu::CpuState{};
  cpu.gpr[3] = handle2_out;
  cpu.gpr[5] = 0;
  cpu.gpr[6] = 1;
  assert(f.invoke(0x0D5u, cpu).success && cpu.gpr[3] == 0u);
  const auto handle2 = f.address_space->read32_be(handle2_out);

  const auto handles_array = f.alloc64();  // 2 x dword
  f.address_space->write32_be(handles_array, handle1);
  f.address_space->write32_be(handles_array + 4u, handle2);

  // WaitAny with a zero timeout: handle1 is already signaled, must succeed
  // immediately reporting index 0 (STATUS_WAIT_0 + 0).
  const auto zero_timeout = f.alloc64();
  f.address_space->write64_be(zero_timeout, 0);

  cpu = cpu::CpuState{};
  cpu.gpr[3] = 2;
  cpu.gpr[4] = handles_array;
  cpu.gpr[5] = 0;  // wait type: any
  cpu.gpr[8] = zero_timeout;
  auto result = f.invoke(0x0FEu, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0u);  // STATUS_WAIT_0 (index 0)

  // WaitAll with a zero timeout: handle2 is never signaled, must time out.
  cpu = cpu::CpuState{};
  cpu.gpr[3] = 2;
  cpu.gpr[4] = handles_array;
  cpu.gpr[5] = 1;  // wait type: all
  cpu.gpr[8] = zero_timeout;
  result = f.invoke(0x0FEu, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0x00000102u);  // STATUS_TIMEOUT
}

void test_nt_create_timer_set_and_cancel() {
  Fixture f;
  const auto handle_out = f.alloc32();

  cpu::CpuState cpu{};
  cpu.gpr[3] = handle_out;
  cpu.gpr[4] = 0;  // object attributes
  cpu.gpr[5] = 1;  // SynchronizationTimer (auto-reset)
  auto result = f.invoke(0x0D7u, cpu);
  assert(result.handled && result.success && cpu.gpr[3] == 0u);
  const auto handle = f.address_space->read32_be(handle_out);
  assert(handle != 0u);

  // NtSetTimerEx with a real future due time (relative, negative 100ns
  // units) must actually fire it via the process's TimerManager, not just
  // record it inertly - this is the real, end-to-end regression for the
  // "timer with due_time > 0 never fired" bug.
  const auto due_time_ptr = f.alloc64();
  constexpr std::int64_t kDelayMs = 80;
  f.address_space->write64_be(due_time_ptr, static_cast<std::uint64_t>(-(kDelayMs * 10'000)));

  cpu = cpu::CpuState{};
  cpu.gpr[3] = handle;
  cpu.gpr[4] = due_time_ptr;
  cpu.gpr[5] = 0;  // no guest callback routine
  cpu.gpr[9] = 0;  // one-shot (no period)
  result = f.invoke(0x0FAu, cpu);
  assert(result.handled && result.success && cpu.gpr[3] == 0u);

  // Wait on the timer handle through the real NtWaitForSingleObjectEx
  // export - proves the timer becomes a genuinely signaled, waitable kernel
  // object once TimerManager fires it, not merely that fire() was called in
  // isolation.
  const auto wait_timeout_ptr = f.alloc64();
  f.address_space->write64_be(wait_timeout_ptr, static_cast<std::uint64_t>(-(2000 * 10'000)));
  cpu = cpu::CpuState{};
  cpu.gpr[3] = handle;
  cpu.gpr[6] = wait_timeout_ptr;
  result = f.invoke(0x0FDu, cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0u);  // STATUS_WAIT_0: actually fired within the 2s bound

  // NtCancelTimer must succeed and leave the timer no longer pending.
  const auto current_state_ptr = f.alloc32();
  cpu = cpu::CpuState{};
  cpu.gpr[3] = handle;
  cpu.gpr[4] = current_state_ptr;
  result = f.invoke(0x0CDu, cpu);
  assert(result.handled && result.success && cpu.gpr[3] == 0u);
}

}  // namespace

int main() {
  std::cout << "Testing xboxkrnl sync exports...\n";

  test_ordinals_are_registered();
  test_nt_create_event_and_wait_single();
  test_nt_create_semaphore_release_and_limit();
  test_nt_create_mutant_uses_real_thread_id_and_ownership();
  test_nt_wait_for_multiple_objects_any_and_all();
  test_nt_create_timer_set_and_cancel();

  std::cout << "All xboxkrnl sync export tests passed!\n";
  return 0;
}
