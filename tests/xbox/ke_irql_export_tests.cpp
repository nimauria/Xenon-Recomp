// KfAcquireSpinLock/KfReleaseSpinLock/KeAcquireSpinLockAtRaisedIrql/
// KeReleaseSpinLockFromRaisedIrql/KeTryToAcquireSpinLockAtRaisedIrql and the
// IRQL/critical-region no-ops. Proves the spin-lock exports provide REAL
// mutual exclusion between concurrent host threads (see
// src/xbox/exports/xboxkrnl_ke_irql_exports.cpp's guest-address-keyed
// std::mutex table) - not just that they are registered.

#include <atomic>
#include <cassert>
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

}  // namespace

int main() {
  std::cout << "\n=== Ke/Kf IRQL and Spin-Lock Export Correctness Tests ===" << std::endl;

  test_registered_and_basic_return_values();
  test_spin_lock_provides_real_mutual_exclusion();
  test_try_acquire_fails_when_held();

  std::cout << "\n\xE2\x9C\x85 All tests passed!" << std::endl;
  return 0;
}
