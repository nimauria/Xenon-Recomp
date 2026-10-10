#include "xenon/xbox/xboxkrnl_ke_irql_exports.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#include "xenon/core/export_registry.hpp"
#include "xbox/exports/sync_events.hpp"

namespace xenon::xbox {
namespace {

using xenon::core::ExportCallContext;
using xenon::logging::events::EventKind;

// A KSPIN_LOCK is one word of guest memory (0 = free, nonzero = held) and the
// title may read or write it directly - inlined lock code, KeInitializeSpinLock
// storing 0, debug checks. So the lock state must live IN that word rather than
// in a host-side table keyed by address: a host mutex never sees a guest-side
// store, and unlocking a mutex from a different thread than the one that locked
// it is undefined behaviour (a title may legally release a lock another thread
// took). Acquisition is an atomic compare-and-swap on the guest word through the
// same reservation protocol as lwarx/stwcx., so it is coherent with any guest
// code touching the word; the holder's thread id is stored as the owner value.
std::uint32_t owner_value(const ExportCallContext& context) {
  return context.thread_id != 0u ? context.thread_id : 1u;
}

bool try_acquire(ExportCallContext& context, cpu::GuestAddress lock_address) {
  // The reservation monitor is a small shared pool whose commit step waits for all
  // in-flight load-reserves; several host threads hammering it at once (a contended
  // lock) degrade to one success per scheduler tick. Serialising just the
  // reserve+commit attempt - never the time the lock is HELD - keeps each attempt
  // uncontended. Guest code doing its own lwarx/stwcx. on the word stays coherent
  // because the attempt still goes through the reservation protocol.
  static std::mutex attempt_guard;
  std::scoped_lock attempt_lock(attempt_guard);
  std::uint32_t current = 0u;
  const auto token = context.memory.reserve32(lock_address, current);
  if (current != 0u) {
    // A load-reserve holds one of the six shared reservation slots until it is
    // stored to or cancelled; leaking it here would starve every later acquire.
    context.memory.cancel_reservation(token);
    return false;
  }
  return context.memory.store_conditional32(lock_address, token, owner_value(context));
}

void acquire(ExportCallContext& context, cpu::GuestAddress lock_address) {
  // Contention on a real spin lock is a few instructions long. Spin, then yield;
  // only a lock held across a long host stall (thousands of failed attempts) falls
  // back to sleeping, so it does not burn a core. (Windows rounds short sleeps up
  // to a scheduler tick, so sleeping any earlier would make ordinary contention crawl.)
  const auto acquire_start = std::chrono::steady_clock::now();
  for (std::uint32_t attempt = 0;; ++attempt) {
    // Test-and-test-and-set: only issue the (comparatively heavy) load-reserve/
    // store-conditional pair when a plain load says the lock looks free, so
    // waiters do not keep starving the holder's own reservation traffic.
    if (context.memory.read32_be(lock_address) == 0u && try_acquire(context, lock_address)) {
      if (logging::events::enabled()) {
        const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - acquire_start)
                                   .count();
        const auto value = static_cast<std::uint32_t>(std::min<long long>(waited_ms, 0xFFFFFFFFll));
        sync_events::record(EventKind::LockAcquired, context, nullptr, lock_address, value,
                            "KeAcquireSpinLock");
      }
      return;
    }
    if (attempt < 64u) continue;
    // The session is shutting down and the holder may never run again: give up
    // rather than hang the join of the thread that is waiting here.
    if ((attempt & 0xFFu) == 0u && context.stopping &&
        context.stopping->load(std::memory_order_acquire)) {
      return;
    }
    if (attempt < 20000u) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

void release(ExportCallContext& context, cpu::GuestAddress lock_address) {
  sync_events::record(EventKind::LockReleased, context, nullptr, lock_address, 0u,
                      "KeReleaseSpinLock");
  context.memory.write32_be(lock_address, 0u);
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
// - always 0 here). Blocks until the lock word in guest memory is acquired.
bool kf_acquire_spin_lock_export(ExportCallContext& context) {
  const auto lock_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  acquire(context, lock_address);
  context.cpu.gpr[3] = 0u;
  return true;
}

// KfReleaseSpinLock (ordinal 0xB4)
// Guest ABI: r3 = PKSPIN_LOCK, r4 = IRQL to restore (ignored, see above) ->
// void.
bool kf_release_spin_lock_export(ExportCallContext& context) {
  const auto lock_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  release(context, lock_address);
  return true;
}

// KeAcquireSpinLockAtRaisedIrql (ordinal 0x4D)
// Guest ABI: r3 = PKSPIN_LOCK -> void. Caller already claims to be at
// DISPATCH_LEVEL; the real mutual-exclusion contract is identical to
// KfAcquireSpinLock's.
bool ke_acquire_spin_lock_at_raised_irql_export(ExportCallContext& context) {
  const auto lock_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  acquire(context, lock_address);
  return true;
}

// KeReleaseSpinLockFromRaisedIrql (ordinal 0x89)
// Guest ABI: r3 = PKSPIN_LOCK -> void.
bool ke_release_spin_lock_from_raised_irql_export(ExportCallContext& context) {
  const auto lock_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  release(context, lock_address);
  return true;
}

// KeTryToAcquireSpinLockAtRaisedIrql (ordinal 0xAE)
// Guest ABI: r3 = PKSPIN_LOCK -> r3 = BOOLEAN (1 if acquired, 0 if already
// held elsewhere).
bool ke_try_to_acquire_spin_lock_at_raised_irql_export(ExportCallContext& context) {
  const auto lock_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  context.cpu.gpr[3] = try_acquire(context, lock_address) ? 1u : 0u;
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
