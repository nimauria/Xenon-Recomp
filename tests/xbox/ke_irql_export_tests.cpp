// KfAcquireSpinLock/KfReleaseSpinLock/KeAcquireSpinLockAtRaisedIrql/
// KeReleaseSpinLockFromRaisedIrql/KeTryToAcquireSpinLockAtRaisedIrql and the
// IRQL/critical-region no-ops. Proves the spin-lock exports provide REAL
// mutual exclusion between concurrent host threads (see
// src/xbox/exports/xboxkrnl_ke_irql_exports.cpp's guest-memory lock word
// (compare-and-swap on the KSPIN_LOCK itself) - not just that they are registered.

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::xbox {
bool register_xboxkrnl_ke_irql_exports(core::ExportRegistry& registry);
}  // namespace xenon::xbox

using namespace xenon;

namespace {

struct Fixture {
  memory::AddressSpace address_space{memory::GuestTranslationMode::Compact};
  core::ExportRegistry registry;

  Fixture() {
    assert(address_space.initialize());
    assert(xbox::register_xboxkrnl_ke_irql_exports(registry));
  }

  core::ExportCallResult invoke(std::uint32_t ordinal, cpu::CpuState& cpu) {
    core::ExportCallContext ctx{cpu, address_space};
    return registry.invoke("xboxkrnl.exe", ordinal, ctx);
  }
};

void test_registered_and_basic_return_values() {
  std::cout << "[TEST] Ke/Kf IRQL exports registered with correct return values..." << std::endl;
  Fixture fx;

  memory::GuestAddress lock_addr{};
  assert(fx.address_space.allocate(4u, 4u, memory::kReadWrite, false, lock_addr));

  cpu::CpuState cpu{};
  assert(fx.invoke(0x05Fu, cpu).success);  // KeEnterCriticalRegion
  assert(fx.invoke(0x07Du, cpu).success);  // KeLeaveCriticalRegion

  cpu = {};
  assert(fx.invoke(0x085u, cpu).success);  // KeRaiseIrqlToDpcLevel
  assert(cpu.gpr[3] == 0u);

  cpu = {};
  cpu.gpr[3] = lock_addr;
  assert(fx.invoke(0x0B1u, cpu).success);  // KfAcquireSpinLock
  assert(cpu.gpr[3] == 0u);
  cpu.gpr[3] = lock_addr;
  cpu.gpr[4] = 0u;
  assert(fx.invoke(0x0B4u, cpu).success);  // KfReleaseSpinLock

  std::cout << "  \xE2\x9C\x93 Ke/Kf IRQL exports registered and return real semantics"
            << std::endl;
}

void test_spin_lock_provides_real_mutual_exclusion() {
  std::cout << "[TEST] KfAcquireSpinLock provides real mutual exclusion across threads..."
            << std::endl;
  Fixture fx;

  memory::GuestAddress lock_addr{};
  assert(fx.address_space.allocate(4u, 4u, memory::kReadWrite, false, lock_addr));

  std::atomic<int> inside_critical_section{0};
  std::atomic<bool> saw_overlap{false};
  constexpr int kIterations = 2000;

  auto worker = [&]() {
    for (int i = 0; i < kIterations; ++i) {
      cpu::CpuState acquire_cpu{};
      acquire_cpu.gpr[3] = lock_addr;
      core::ExportCallContext acquire_ctx{acquire_cpu, fx.address_space};
      fx.registry.invoke("xboxkrnl.exe", 0x0B1u, acquire_ctx);

      if (inside_critical_section.fetch_add(1, std::memory_order_relaxed) != 0) {
        saw_overlap.store(true, std::memory_order_relaxed);
      }
      inside_critical_section.fetch_sub(1, std::memory_order_relaxed);

      cpu::CpuState release_cpu{};
      release_cpu.gpr[3] = lock_addr;
      core::ExportCallContext release_ctx{release_cpu, fx.address_space};
      fx.registry.invoke("xboxkrnl.exe", 0x0B4u, release_ctx);
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back(worker);
  for (auto& t : threads) t.join();

  assert(!saw_overlap.load());

  std::cout << "  \xE2\x9C\x93 No two threads were ever simultaneously inside the spin-lock's "
               "critical section"
            << std::endl;
}

void test_try_acquire_fails_when_held() {
  std::cout << "[TEST] KeTryToAcquireSpinLockAtRaisedIrql fails when already held..." << std::endl;
  Fixture fx;

  memory::GuestAddress lock_addr{};
  assert(fx.address_space.allocate(4u, 4u, memory::kReadWrite, false, lock_addr));

  cpu::CpuState acquire_cpu{};
  acquire_cpu.gpr[3] = lock_addr;
  assert(fx.invoke(0x04Du, acquire_cpu).success);  // KeAcquireSpinLockAtRaisedIrql

  cpu::CpuState try_cpu{};
  try_cpu.gpr[3] = lock_addr;
  assert(fx.invoke(0x0AEu, try_cpu).success);  // KeTryToAcquireSpinLockAtRaisedIrql
  assert(try_cpu.gpr[3] == 0u);                // already held -> fails

  cpu::CpuState release_cpu{};
  release_cpu.gpr[3] = lock_addr;
  assert(fx.invoke(0x089u, release_cpu).success);  // KeReleaseSpinLockFromRaisedIrql

  cpu::CpuState try_again_cpu{};
  try_again_cpu.gpr[3] = lock_addr;
  assert(fx.invoke(0x0AEu, try_again_cpu).success);
  assert(try_again_cpu.gpr[3] == 1u);  // now free -> succeeds

  std::cout << "  \xE2\x9C\x93 KeTryToAcquireSpinLockAtRaisedIrql reflects real lock state"
            << std::endl;
}

// Regression (Ace Combat 6 vsync callback stalled in KfAcquireSpinLock): a
// KSPIN_LOCK is a word of guest memory. Its state must be visible there, code that
// releases it by storing 0 itself must free it for the exports, and a lock taken
// on one thread must be releasable from another - all impossible when the lock
// lived in a host mutex table that guest memory never touched.
void test_lock_state_lives_in_the_guest_word() {
  std::cout << "[TEST] Spin-lock state lives in the guest lock word..." << std::endl;
  Fixture fx;

  memory::GuestAddress lock_addr{};
  assert(fx.address_space.allocate(4u, 4u, memory::kReadWrite, false, lock_addr));
  fx.address_space.write32_be(lock_addr, 0u);

  cpu::CpuState acquire_cpu{};
  acquire_cpu.gpr[3] = lock_addr;
  core::ExportCallContext acquire_ctx{acquire_cpu, fx.address_space, 0u, 7u};
  assert(fx.registry.invoke("xboxkrnl.exe", 0x0B1u, acquire_ctx).success);
  assert(fx.address_space.read32_be(lock_addr) == 7u && "held: word holds the owner thread id");

  // Held, so a try-acquire from another thread fails.
  cpu::CpuState try_cpu{};
  try_cpu.gpr[3] = lock_addr;
  core::ExportCallContext try_ctx{try_cpu, fx.address_space, 0u, 8u};
  assert(fx.registry.invoke("xboxkrnl.exe", 0x0AEu, try_ctx).success);
  assert(try_cpu.gpr[3] == 0u);
  assert(fx.address_space.read32_be(lock_addr) == 7u);

  // Released by a DIFFERENT thread than the acquirer: legal, must not be UB and
  // must free the word.
  cpu::CpuState release_cpu{};
  release_cpu.gpr[3] = lock_addr;
  core::ExportCallContext release_ctx{release_cpu, fx.address_space, 0u, 8u};
  assert(fx.registry.invoke("xboxkrnl.exe", 0x0B4u, release_ctx).success);
  assert(fx.address_space.read32_be(lock_addr) == 0u);

  // Held again, then released by the title itself storing 0 (inlined unlock).
  cpu::CpuState again_cpu{};
  again_cpu.gpr[3] = lock_addr;
  core::ExportCallContext again_ctx{again_cpu, fx.address_space, 0u, 9u};
  assert(fx.registry.invoke("xboxkrnl.exe", 0x0B1u, again_ctx).success);
  assert(fx.address_space.read32_be(lock_addr) == 9u);
  fx.address_space.write32_be(lock_addr, 0u);
  cpu::CpuState after_cpu{};
  after_cpu.gpr[3] = lock_addr;
  core::ExportCallContext after_ctx{after_cpu, fx.address_space, 0u, 10u};
  assert(fx.registry.invoke("xboxkrnl.exe", 0x0AEu, after_ctx).success);
  assert(after_cpu.gpr[3] == 1u && "a lock the title unlocked itself is acquirable");

  // A lock the title pre-set to held blocks acquisition until it clears it.
  fx.address_space.write32_be(lock_addr, 0xDEADu);
  std::atomic<bool> acquired{false};
  std::thread waiter([&] {
    cpu::CpuState cpu{};
    cpu.gpr[3] = lock_addr;
    core::ExportCallContext ctx{cpu, fx.address_space, 0u, 11u};
    fx.registry.invoke("xboxkrnl.exe", 0x0B1u, ctx);
    acquired.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(!acquired.load() && "must wait while the guest word is nonzero");
  fx.address_space.write32_be(lock_addr, 0u);
  waiter.join();
  assert(acquired.load());
  assert(fx.address_space.read32_be(lock_addr) == 11u);

  std::cout << "  \xE2\x9C\x93 Lock word is the lock: visible, guest-releasable, cross-thread releasable"
            << std::endl;
}

// Regression: session shutdown hung joining the GPU pump thread, which sat in
// KfAcquireSpinLock behind a lock whose holder was blocked forever. A spin-wait must
// give up once the session is stopping.
void test_spin_wait_aborts_when_session_is_stopping() {
  std::cout << "[TEST] A spin-lock wait aborts when the session is stopping..." << std::endl;
  Fixture fx;

  memory::GuestAddress lock_addr{};
  assert(fx.address_space.allocate(4u, 4u, memory::kReadWrite, false, lock_addr));
  fx.address_space.write32_be(lock_addr, 0xDEADu);  // held by someone who never returns

  std::atomic<bool> stopping{false};
  std::atomic<bool> returned{false};
  std::thread waiter([&] {
    cpu::CpuState cpu{};
    cpu.gpr[3] = lock_addr;
    core::ExportCallContext ctx{cpu, fx.address_space, 0u, 5u, &stopping};
    fx.registry.invoke("xboxkrnl.exe", 0x0B1u, ctx);
    returned.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(!returned.load() && "still waiting while the session runs");
  stopping.store(true);
  for (int i = 0; i < 500 && !returned.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(returned.load() && "must return once the session is stopping");
  waiter.join();
  assert(fx.address_space.read32_be(lock_addr) == 0xDEADu && "the foreign lock is untouched");

  std::cout << "  \xE2\x9C\x93 Spin-lock wait is abortable by session shutdown" << std::endl;
}

}  // namespace

int main() {
  std::cout << "\n=== Ke/Kf IRQL and Spin-Lock Export Correctness Tests ===" << std::endl;

  test_registered_and_basic_return_values();
  test_spin_lock_provides_real_mutual_exclusion();
  test_try_acquire_fails_when_held();
  test_lock_state_lives_in_the_guest_word();
  test_spin_wait_aborts_when_session_is_stopping();

  std::cout << "\n\xE2\x9C\x85 All tests passed!" << std::endl;
  return 0;
}
