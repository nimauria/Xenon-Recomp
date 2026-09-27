#include "xenon/xbox/xboxkrnl_ke_irql_exports.hpp"

#include <mutex>
#include <unordered_map>

#include "xenon/core/export_registry.hpp"

namespace xenon::xbox {
namespace {

using xenon::core::ExportCallContext;

// Real KSPIN_LOCK is just a ULONG_PTR in guest memory (0 = free, nonzero =
// held), and real hardware busy-waits on it directly. Xenon instead backs
// each distinct guest lock address with a real host std::mutex, keyed by
// address - this preserves the one guest-visible contract that matters
// (mutual exclusion between the real concurrent host threads Xenon runs
// guest threads as; see docs/runtime/RUNTIME_SESSION.md's guest thread
// model), without needing to replicate lwarx/stwcx-style busy-waiting by
// hand. A non-recursive std::mutex matches real KSPIN_LOCK semantics
// exactly: re-acquiring the same spin lock on the same thread without
// releasing it first deadlocks/bugchecks on real hardware too, so this is
// not a Xenon-specific limitation.
std::mutex& lock_for(cpu::GuestAddress address) {
  static std::mutex table_mutex;
  static std::unordered_map<cpu::GuestAddress, std::unique_ptr<std::mutex>> locks;

  std::scoped_lock guard(table_mutex);
  auto it = locks.find(address);
  if (it == locks.end()) {
    it = locks.emplace(address, std::make_unique<std::mutex>()).first;
  }
  return *it->second;
}

// KeEnterCriticalRegion / KeLeaveCriticalRegion (ordinals 0x5F / 0x7D)
// Guest ABI: no parameters, no return value. Real semantics disable/
// re-enable normal-priority APC delivery to the current thread. Xenon has
// no asynchronous APC delivery into running guest code (compiled guest
// functions run straight through; nothing interrupts them mid-function to
// deliver an APC), so there is nothing for these to actually suspend -
// making them genuine no-ops here, not an approximation of missing
// behavior.
bool ke_enter_critical_region_export(ExportCallContext&) { return true; }
bool ke_leave_critical_region_export(ExportCallContext&) { return true; }

// KeRaiseIrqlToDpcLevel (ordinal 0x85)
// Guest ABI: no parameters -> r3 = previous IRQL. Xenon does not model real
// interrupt-level preemption (see the critical-region note above), so no
// code anywhere reads back a "current IRQL" - returning a constant
// PASSIVE_LEVEL (0) as the previous value is a faithful stand-in for the
// common case (nothing else already raised it) with no observable
// consequence given nothing consumes real IRQL state.
bool ke_raise_irql_to_dpc_level_export(ExportCallContext& context) {
  context.cpu.gpr[3] = 0u;
  return true;
}

// KfLowerIrql (ordinal 0xB3)
// Guest ABI: r3 = new IRQL -> void. See KeRaiseIrqlToDpcLevel's note; the
// requested IRQL has nothing to actually apply to.
bool kf_lower_irql_export(ExportCallContext&) { return true; }

// KfAcquireSpinLock (ordinal 0xB1)
// Guest ABI: r3 = PKSPIN_LOCK -> r3 = previous IRQL (see KeRaiseIrqlToDpcLevel
// - always 0 here). Blocks until the real host mutex backing this guest
// lock address is acquired.
bool kf_acquire_spin_lock_export(ExportCallContext& context) {
  const auto lock_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  lock_for(lock_address).lock();
  context.cpu.gpr[3] = 0u;
  return true;
}

// KfReleaseSpinLock (ordinal 0xB4)
// Guest ABI: r3 = PKSPIN_LOCK, r4 = IRQL to restore (ignored, see above) ->
// void.
bool kf_release_spin_lock_export(ExportCallContext& context) {
  const auto lock_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  lock_for(lock_address).unlock();
  return true;
}

// KeAcquireSpinLockAtRaisedIrql (ordinal 0x4D)
// Guest ABI: r3 = PKSPIN_LOCK -> void. Caller already claims to be at
// DISPATCH_LEVEL; the real mutual-exclusion contract is identical to
// KfAcquireSpinLock's.
bool ke_acquire_spin_lock_at_raised_irql_export(ExportCallContext& context) {
  const auto lock_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  lock_for(lock_address).lock();
  return true;
}

// KeReleaseSpinLockFromRaisedIrql (ordinal 0x89)
// Guest ABI: r3 = PKSPIN_LOCK -> void.
bool ke_release_spin_lock_from_raised_irql_export(ExportCallContext& context) {
  const auto lock_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  lock_for(lock_address).unlock();
  return true;
}

// KeTryToAcquireSpinLockAtRaisedIrql (ordinal 0xAE)
// Guest ABI: r3 = PKSPIN_LOCK -> r3 = BOOLEAN (1 if acquired, 0 if already
// held elsewhere).
bool ke_try_to_acquire_spin_lock_at_raised_irql_export(ExportCallContext& context) {
  const auto lock_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  context.cpu.gpr[3] = lock_for(lock_address).try_lock() ? 1u : 0u;
  return true;
}

struct KeIrqlExportSpec {
  std::uint32_t ordinal;
  const char* name;
  core::ExportHandler handler;
};

const KeIrqlExportSpec kKeIrqlExports[] = {
    {0x05Fu, "KeEnterCriticalRegion", &ke_enter_critical_region_export},
    {0x07Du, "KeLeaveCriticalRegion", &ke_leave_critical_region_export},
    {0x085u, "KeRaiseIrqlToDpcLevel", &ke_raise_irql_to_dpc_level_export},
    {0x0B3u, "KfLowerIrql", &kf_lower_irql_export},
    {0x0B1u, "KfAcquireSpinLock", &kf_acquire_spin_lock_export},
    {0x0B4u, "KfReleaseSpinLock", &kf_release_spin_lock_export},
    {0x04Du, "KeAcquireSpinLockAtRaisedIrql", &ke_acquire_spin_lock_at_raised_irql_export},
    {0x089u, "KeReleaseSpinLockFromRaisedIrql", &ke_release_spin_lock_from_raised_irql_export},
    {0x0AEu, "KeTryToAcquireSpinLockAtRaisedIrql",
     &ke_try_to_acquire_spin_lock_at_raised_irql_export},
};

}  // namespace

bool register_xboxkrnl_ke_irql_exports(core::ExportRegistry& registry) {
  for (const auto& spec : kKeIrqlExports) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = spec.name;
    descriptor.ordinal = spec.ordinal;
    descriptor.handler = spec.handler;
    descriptor.requirement = core::ExportRequirement::Required;

    if (!registry.register_export(std::move(descriptor))) {
      return false;
    }
  }

  return true;
}

}  // namespace xenon::xbox
