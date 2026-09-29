// Rtl*CriticalSection exports. Drives each export through
// core::ExportRegistry::invoke() exactly as a guest thunk would.
//
// Real-world context: RtlInitializeCriticalSection (ordinal 302 / 0x12E) is
// the next real export AC6's boot path calls once the process-type exports
// were fixed (real guest address 0x823d009c) - a correctly-recognized-but-
// previously-unimplemented import, per src/core/session.cpp's Trap path
// (STATUS_PROCEDURE_NOT_FOUND).

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xboxkrnl_rtl_critical_section_exports.hpp"

using namespace xenon;

namespace {

constexpr std::uint32_t kOrdInitialize = 0x12Eu;
constexpr std::uint32_t kOrdInitializeAndSpinCount = 0x12Fu;
constexpr std::uint32_t kOrdEnter = 0x125u;
constexpr std::uint32_t kOrdTryEnter = 0x141u;
constexpr std::uint32_t kOrdLeave = 0x130u;
constexpr std::uint32_t kLockCountOffset = 0x10u;
constexpr std::uint32_t kRecursionCountOffset = 0x14u;
constexpr std::uint32_t kOwningThreadOffset = 0x18u;

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
    assert(xbox::register_xboxkrnl_rtl_critical_section_exports(registry, *process));
  }

  [[nodiscard]] core::ExportCallResult invoke(std::uint32_t ordinal, cpu::CpuState& cpu,
                                              std::uint32_t thread_id = 1u) {
    core::ExportCallContext call{cpu, *address_space, 0, thread_id};
    return registry.invoke("xboxkrnl", ordinal, call);
  }

  [[nodiscard]] memory::GuestAddress alloc_cs() {
    memory::GuestAddress addr{};
    assert(address_space->allocate(0x1Cu, 4u, memory::kReadWrite, false, addr));
    return addr;
  }
};

void test_ordinals_are_registered() {
  Fixture fixture;
  assert(fixture.registry.contains("xboxkrnl.exe", kOrdInitialize));
  assert(fixture.registry.contains("xboxkrnl", "RtlInitializeCriticalSection"));
  assert(fixture.registry.contains("xboxkrnl", kOrdInitializeAndSpinCount));
  assert(fixture.registry.contains("xboxkrnl", "RtlInitializeCriticalSectionAndSpinCount"));
  assert(fixture.registry.contains("xboxkrnl", kOrdEnter));
  assert(fixture.registry.contains("xboxkrnl", "RtlEnterCriticalSection"));
  assert(fixture.registry.contains("xboxkrnl", kOrdTryEnter));
  assert(fixture.registry.contains("xboxkrnl", "RtlTryEnterCriticalSection"));
  assert(fixture.registry.contains("xboxkrnl", kOrdLeave));
  assert(fixture.registry.contains("xboxkrnl", "RtlLeaveCriticalSection"));
}

// Real Xbox 360 initial field values (verified against xenia's
// xeRtlInitializeCriticalSection): lock_count=-1, recursion_count=0,
// owning_thread=0.
void test_initialize_sets_real_initial_state() {
  Fixture fixture;
  const auto cs = fixture.alloc_cs();

  cpu::CpuState cpu{};
  cpu.gpr[3] = cs;
  assert(fixture.invoke(kOrdInitialize, cpu).success);

  assert(static_cast<std::int32_t>(fixture.address_space->read32_be(cs + kLockCountOffset)) == -1);
  assert(fixture.address_space->read32_be(cs + kRecursionCountOffset) == 0u);
  assert(fixture.address_space->read32_be(cs + kOwningThreadOffset) == 0u);
}

// Uncontended Enter must acquire immediately; Leave must fully release.
void test_enter_leave_uncontended() {
  Fixture fixture;
  const auto cs = fixture.alloc_cs();
  cpu::CpuState init_cpu{};
  init_cpu.gpr[3] = cs;
  assert(fixture.invoke(kOrdInitialize, init_cpu).success);

  cpu::CpuState enter_cpu{};
  enter_cpu.gpr[3] = cs;
  assert(fixture.invoke(kOrdEnter, enter_cpu, /*thread_id=*/7u).success);
  assert(fixture.address_space->read32_be(cs + kOwningThreadOffset) == 7u);
  assert(fixture.address_space->read32_be(cs + kRecursionCountOffset) == 1u);

  cpu::CpuState leave_cpu{};
  leave_cpu.gpr[3] = cs;
  assert(fixture.invoke(kOrdLeave, leave_cpu, /*thread_id=*/7u).success);
  assert(fixture.address_space->read32_be(cs + kOwningThreadOffset) == 0u);
  assert(fixture.address_space->read32_be(cs + kRecursionCountOffset) == 0u);
}

// The same thread re-entering its own held critical section must succeed
// (recursion), and only fully release after a matching number of Leaves.
void test_recursive_enter_leave() {
  Fixture fixture;
  const auto cs = fixture.alloc_cs();
  cpu::CpuState init_cpu{};
  init_cpu.gpr[3] = cs;
  assert(fixture.invoke(kOrdInitialize, init_cpu).success);

  constexpr std::uint32_t kThread = 3u;
  for (int i = 0; i < 3; ++i) {
    cpu::CpuState cpu{};
    cpu.gpr[3] = cs;
    assert(fixture.invoke(kOrdEnter, cpu, kThread).success);
  }
  assert(fixture.address_space->read32_be(cs + kRecursionCountOffset) == 3u);
  assert(fixture.address_space->read32_be(cs + kOwningThreadOffset) == kThread);

  for (int i = 0; i < 2; ++i) {
    cpu::CpuState cpu{};
    cpu.gpr[3] = cs;
    assert(fixture.invoke(kOrdLeave, cpu, kThread).success);
    // Still held - recursion count > 0.
    assert(fixture.address_space->read32_be(cs + kOwningThreadOffset) == kThread);
  }
  cpu::CpuState final_leave{};
  final_leave.gpr[3] = cs;
  assert(fixture.invoke(kOrdLeave, final_leave, kThread).success);
  assert(fixture.address_space->read32_be(cs + kOwningThreadOffset) == 0u);
  assert(fixture.address_space->read32_be(cs + kRecursionCountOffset) == 0u);
}

// TryEnter must succeed when free (or already owned by the caller) and fail
// (return 0, non-blocking) when genuinely held by another thread.
void test_try_enter() {
  Fixture fixture;
  const auto cs = fixture.alloc_cs();
  cpu::CpuState init_cpu{};
  init_cpu.gpr[3] = cs;
  assert(fixture.invoke(kOrdInitialize, init_cpu).success);

  cpu::CpuState try1{};
  try1.gpr[3] = cs;
  assert(fixture.invoke(kOrdTryEnter, try1, /*thread_id=*/1u).success);
  assert(try1.gpr[3] == 1u && "TryEnter on a free critical section must succeed");

  // Same thread recursively.
  cpu::CpuState try2{};
  try2.gpr[3] = cs;
  assert(fixture.invoke(kOrdTryEnter, try2, /*thread_id=*/1u).success);
  assert(try2.gpr[3] == 1u && "TryEnter must allow recursive acquire by the owning thread");

  // A different thread must fail, not block.
  cpu::CpuState try3{};
  try3.gpr[3] = cs;
  assert(fixture.invoke(kOrdTryEnter, try3, /*thread_id=*/2u).success);
  assert(try3.gpr[3] == 0u && "TryEnter must fail (not block) when held by another thread");
}

// The real correctness test: a second thread's Enter() must actually block
// until the first thread's Leave() releases the lock, and mutual exclusion
// must genuinely hold under real concurrent host-thread execution (not just
// single-threaded field bookkeeping).
void test_enter_blocks_and_wakes_across_real_threads() {
  Fixture fixture;
  const auto cs = fixture.alloc_cs();
  cpu::CpuState init_cpu{};
  init_cpu.gpr[3] = cs;
  assert(fixture.invoke(kOrdInitialize, init_cpu).success);

  std::atomic<int> critical_occupants{0};
  std::atomic<bool> saw_overlap{false};
  std::atomic<int> completed{0};
  constexpr int kThreads = 4;
  constexpr int kIterationsPerThread = 25;

  auto worker = [&](std::uint32_t thread_id) {
    for (int i = 0; i < kIterationsPerThread; ++i) {
      cpu::CpuState enter_cpu{};
      enter_cpu.gpr[3] = cs;
      core::ExportCallContext enter_call{enter_cpu, *fixture.address_space, 0, thread_id};
      assert(fixture.registry.invoke("xboxkrnl", kOrdEnter, enter_call).success);

      const auto occupants = critical_occupants.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (occupants > 1) saw_overlap.store(true, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      critical_occupants.fetch_sub(1, std::memory_order_acq_rel);

      cpu::CpuState leave_cpu{};
      leave_cpu.gpr[3] = cs;
      core::ExportCallContext leave_call{leave_cpu, *fixture.address_space, 0, thread_id};
      assert(fixture.registry.invoke("xboxkrnl", kOrdLeave, leave_call).success);
    }
    completed.fetch_add(1, std::memory_order_relaxed);
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    // Thread ids start at 1 elsewhere in this codebase (0 means "unlocked");
    // mirror that here too.
    threads.emplace_back(worker, static_cast<std::uint32_t>(t + 1));
  }
  for (auto& t : threads) t.join();

  assert(completed.load() == kThreads);
  assert(!saw_overlap.load() &&
         "RtlEnterCriticalSection must provide real mutual exclusion across real host threads");

  // The critical section must end fully released.
  assert(fixture.address_space->read32_be(cs + kOwningThreadOffset) == 0u);
  assert(fixture.address_space->read32_be(cs + kRecursionCountOffset) == 0u);
}

template <typename Predicate>
bool wait_until(Predicate&& predicate, std::chrono::milliseconds limit = std::chrono::seconds(10)) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// Regression (AC6 bring-up): a woken waiter used to reset LockCount to 0 when it
// took the section, dropping every other waiter's registration. With two waiters
// the first Leave woke one, the second Leave then saw "no waiters" and never
// signalled the other, which slept forever (the whole test binary hung). NT hands
// ownership to the woken waiter without touching LockCount.
void test_lock_count_survives_handoff_with_multiple_waiters() {
  Fixture fixture;
  const auto cs = fixture.alloc_cs();
  cpu::CpuState init_cpu{};
  init_cpu.gpr[3] = cs;
  assert(fixture.invoke(kOrdInitialize, init_cpu).success);

  const auto lock_count = [&] {
    return static_cast<std::int32_t>(fixture.address_space->read32_be(cs + kLockCountOffset));
  };

  cpu::CpuState enter1{};
  enter1.gpr[3] = cs;
  assert(fixture.invoke(kOrdEnter, enter1, 1u).success);
  assert(lock_count() == 0);

  std::atomic<int> acquired{0};
  std::atomic<int> finished{0};
  std::atomic<bool> release{false};
  const auto waiter = [&](std::uint32_t thread_id) {
    cpu::CpuState enter{};
    enter.gpr[3] = cs;
    core::ExportCallContext enter_call{enter, *fixture.address_space, 0, thread_id};
    assert(fixture.registry.invoke("xboxkrnl", kOrdEnter, enter_call).success);
    acquired.fetch_add(1);
    while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    cpu::CpuState leave{};
    leave.gpr[3] = cs;
    core::ExportCallContext leave_call{leave, *fixture.address_space, 0, thread_id};
    assert(fixture.registry.invoke("xboxkrnl", kOrdLeave, leave_call).success);
    finished.fetch_add(1);
  };
  std::thread t2(waiter, 2u);
  std::thread t3(waiter, 3u);
  assert(wait_until([&] { return lock_count() == 2; }) && "both waiters must register");

  cpu::CpuState leave1{};
  leave1.gpr[3] = cs;
  assert(fixture.invoke(kOrdLeave, leave1, 1u).success);
  assert(wait_until([&] { return acquired.load() == 1; }));
  // One waiter owns it now; the other is still registered.
  assert(lock_count() == 1 && "the remaining waiter's registration must survive the hand-off");
  assert(fixture.address_space->read32_be(cs + kRecursionCountOffset) == 1u);

  // While the section is owned and a waiter is registered, TryEnter must fail.
  cpu::CpuState try_enter{};
  try_enter.gpr[3] = cs;
  assert(fixture.invoke(kOrdTryEnter, try_enter, 4u).success);
  assert(try_enter.gpr[3] == 0u);

  release.store(true);
  const bool all_done = wait_until([&] { return finished.load() == 2; });
  if (!all_done) {
    std::cerr << "a critical-section waiter was never woken (lost wakeup)\n";
    std::abort();
  }
  t2.join();
  t3.join();
  assert(lock_count() == -1);
  assert(fixture.address_space->read32_be(cs + kOwningThreadOffset) == 0u);
}

// Between a Leave() and the woken waiter taking over, the section is unowned but
// still committed to that waiter: TryEnter must not steal it.
void test_try_enter_does_not_steal_a_pending_handoff() {
  Fixture fixture;
  const auto cs = fixture.alloc_cs();
  cpu::CpuState init_cpu{};
  init_cpu.gpr[3] = cs;
  assert(fixture.invoke(kOrdInitialize, init_cpu).success);

  // Model "T1 held it, T2 registered as a waiter, T1 left": owner 0, LockCount 0.
  fixture.address_space->write32_be(cs + kOwningThreadOffset, 0u);
  fixture.address_space->write32_be(cs + kRecursionCountOffset, 0u);
  fixture.address_space->write32_be(cs + kLockCountOffset, 0u);

  cpu::CpuState try_enter{};
  try_enter.gpr[3] = cs;
  assert(fixture.invoke(kOrdTryEnter, try_enter, 3u).success);
  assert(try_enter.gpr[3] == 0u && "TryEnter must not take a section that is mid hand-off");
  assert(fixture.address_space->read32_be(cs + kOwningThreadOffset) == 0u);
}

}  // namespace

int main() {
  std::cout << "Testing xboxkrnl Rtl*CriticalSection exports...\n";

  test_ordinals_are_registered();
  test_initialize_sets_real_initial_state();
  test_enter_leave_uncontended();
  test_recursive_enter_leave();
  test_try_enter();
  test_enter_blocks_and_wakes_across_real_threads();
  test_lock_count_survives_handoff_with_multiple_waiters();
  test_try_enter_does_not_steal_a_pending_handoff();

  std::cout << "All Rtl*CriticalSection export tests passed!\n";
  return 0;
}
