#include "xenon/xbox/xboxkrnl_rtl_critical_section_exports.hpp"

#include <cstdint>
#include <mutex>
#include <string>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/wait.hpp"
#include "xenon/logging/logger.hpp"
#include "xenon/xbox/xbox_time_convert.hpp"
#include "xenon/xbox/xex_dispatcher_header.hpp"

namespace xenon::xbox {
namespace {

using xenon::core::ExportCallContext;

constexpr std::uint32_t kLockCountOffset = 0x10u;
constexpr std::uint32_t kRecursionCountOffset = 0x14u;
constexpr std::uint32_t kOwningThreadOffset = 0x18u;
// The X_DISPATCH_HEADER's per-type byte at offset 0x1 (see
// xex_dispatcher_header.hpp: "per-type flags union - not read here" by
// resolve_dispatcher_object()) is repurposed by real Xbox 360 critical
// sections to hold (spin_count + 255) / 256, clamped to 255 - verified
// against xenia's xeRtlInitializeCriticalSectionAndSpinCount. Xenon does not
// itself spin (see rtl_enter_critical_section_export()'s own comment on why
// a real host mutex + blocking wait is used instead), so this stored value
// is guest-ABI-compatible bookkeeping only - written so a title reading its
// own critical section's spin count back sees the real, expected value.
constexpr std::uint32_t kSpinCountByteOffset = 0x1u;

// Xenon's chosen "owning thread" identity for a critical section:
// ExportCallContext::thread_id, the same real, stable, nonzero-for-every-
// real-guest-thread identifier KeWaitForSingleObject/mutant ownership
// already use (kernel::KernelThread's thread_id_ counter starts at 1, so 0
// is never a real thread's id and unambiguously means "unlocked" here).
// This is Xenon's own internal thread identity, not literally the real
// hardware's guest PKTHREAD pointer (xenia's XThread::GetCurrentThread()->
// guest_object()) - no evidence any title inspects this field's exact bit
// pattern (it is real kernel-internal Windows CRITICAL_SECTION-equivalent
// bookkeeping on real hardware too), and using a value already proven
// unique-and-stable per guest thread elsewhere in this codebase is more
// robust than fabricating a synthetic guest-visible thread-object address
// with no real backing structure.
std::uint32_t owning_thread_marker(const ExportCallContext& context) {
  return context.thread_id;
}

// Resolves the critical section's own X_DISPATCH_HEADER as a real,
// waitable kernel::KernelEvent (auto-reset, matching real hardware's
// EventSynchronization type) - the header is reused as-is for this, exactly
// as xenia's own reference implementation reuses the same header bytes for
// KeWaitForSingleObject/KeSetEvent rather than allocating a separate object.
std::shared_ptr<kernel::KernelEvent> resolve_wait_event(kernel::KernelProcess& process,
                                                         ExportCallContext& context,
                                                         cpu::GuestAddress cs_address,
                                                         const char* caller) {
  std::string error;
  auto object = resolve_dispatcher_object(process, context.memory, cs_address, &error);
  if (!object || object->type() != kernel::ObjectType::Event) {
    xenon::logging::Logger::instance().log_if_enabled(
        xenon::logging::Level::Warning, "rtl_critical_section", [&] {
          return std::string(caller) + ": failed to resolve critical section wait object: " +
                (object ? "resolved object is not an Event" : error);
        });
    return nullptr;
  }
  return std::static_pointer_cast<kernel::KernelEvent>(object);
}

}  // namespace

// RtlInitializeCriticalSection (ordinal 0x12E)
// Guest ABI: r3 = X_RTL_CRITICAL_SECTION guest pointer -> void.
bool rtl_initialize_critical_section_export(kernel::KernelProcess&,
                                            ExportCallContext& context) {
  const auto cs = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  if (cs == 0u) return true;

  initialize_dispatch_header(context.memory, cs, DispatchObjectType::EventSynchronization, 0u);
  context.memory.write32_be(cs + kLockCountOffset, static_cast<std::uint32_t>(-1));
  context.memory.write32_be(cs + kRecursionCountOffset, 0u);
  context.memory.write32_be(cs + kOwningThreadOffset, 0u);
  return true;
}

// RtlInitializeCriticalSectionAndSpinCount (ordinal 0x12F)
// Guest ABI: r3 = X_RTL_CRITICAL_SECTION guest pointer, r4 = spin count ->
// r3 = NTSTATUS (always STATUS_SUCCESS - real hardware only fails this on
// resource exhaustion allocating kernel-mode debug data Xenon does not
// model).
bool rtl_initialize_critical_section_and_spin_count_export(kernel::KernelProcess& process,
                                                            ExportCallContext& context) {
  const auto cs = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto spin_count = static_cast<std::uint32_t>(context.cpu.gpr[4]);
  if (cs == 0u) {
    context.cpu.gpr[3] = 0xC000000Du;  // STATUS_INVALID_PARAMETER
    return true;
  }

  if (!rtl_initialize_critical_section_export(process, context)) return false;
  std::uint32_t spin_count_div_256 = (spin_count + 255u) >> 8u;
  if (spin_count_div_256 > 255u) spin_count_div_256 = 255u;
  context.memory.write8(cs + kSpinCountByteOffset, static_cast<std::uint8_t>(spin_count_div_256));
  context.cpu.gpr[3] = 0u;  // STATUS_SUCCESS
  return true;
}

// RtlEnterCriticalSection (ordinal 0x125)
// Guest ABI: r3 = X_RTL_CRITICAL_SECTION guest pointer -> void (blocks the
// calling guest thread until acquired).
//
// Real hardware spins for header.absolute*256 iterations on a lock-free
// atomic compare-exchange before falling back to a real kernel wait (see
// xenia's RtlEnterCriticalSection_entry, independently verified). Xenon
// instead guards the guest-visible lock_count/recursion_count/owning_thread
// fields with kernel::KernelProcess::critical_section_mutex() (one mutex per
// process, held only for the brief field read-modify-write below, never
// across the actual blocking wait) and falls back to the same real
// kernel::wait_for_single_object()/KernelEvent machinery
// KeWaitForSingleObject/KeSetEvent already use on contention - Xenon's 1:1
// guest-thread:host-thread model means a host mutex is already exactly as
// correct as a spin-then-atomic-CAS loop for mutual exclusion, without
// needing atomic compare-exchange primitives on guest memory itself. The
// guest-visible field semantics (recursion counting, lock_count as a
// waiter-count-ish value, owning_thread identity - see
// owning_thread_marker()'s own comment) match the verified reference
// exactly; only the internal blocking mechanism differs.
bool rtl_enter_critical_section_export(kernel::KernelProcess& process,
                                       ExportCallContext& context) {
  const auto cs = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  if (cs == 0u) return true;
  const auto self = owning_thread_marker(context);

  for (;;) {
    bool acquired = false;
    bool must_wait = false;
    {
      std::lock_guard<std::mutex> lock(process.critical_section_mutex());
      const auto owner = context.memory.read32_be(cs + kOwningThreadOffset);
      if (owner == self) {
        // Already ours - recursive acquire.
        context.memory.write32_be(
            cs + kRecursionCountOffset,
            context.memory.read32_be(cs + kRecursionCountOffset) + 1u);
        context.memory.write32_be(
            cs + kLockCountOffset, context.memory.read32_be(cs + kLockCountOffset) + 1u);
        acquired = true;
      } else if (owner == 0u) {
        // Uncontended - take it.
        context.memory.write32_be(cs + kOwningThreadOffset, self);
        context.memory.write32_be(cs + kRecursionCountOffset, 1u);
        context.memory.write32_be(cs + kLockCountOffset, 0u);
        acquired = true;
      } else {
        // Contended - register as a waiter and block below, outside the lock.
        context.memory.write32_be(
            cs + kLockCountOffset, context.memory.read32_be(cs + kLockCountOffset) + 1u);
        must_wait = true;
      }
    }
    if (acquired) return true;
    if (!must_wait) continue;  // unreachable, kept for clarity

    auto event = resolve_wait_event(process, context, cs, "RtlEnterCriticalSection");
    if (!event) {
      // Cannot resolve a real wait object - fail safe by treating this as an
      // immediate (unsynchronized) acquire rather than hanging the guest
      // thread forever, and undo the waiter registration above.
      std::lock_guard<std::mutex> lock(process.critical_section_mutex());
      context.memory.write32_be(
          cs + kLockCountOffset, context.memory.read32_be(cs + kLockCountOffset) - 1u);
      context.memory.write32_be(cs + kOwningThreadOffset, self);
      context.memory.write32_be(cs + kRecursionCountOffset, 1u);
      return true;
    }
    // The exact WaitResult is intentionally not branched on: the loop
    // re-validates ownership under the lock regardless of how the wait
    // ended (Success, or the practically-unreachable-with-an-infinite-
    // timeout Timeout/Abandoned/Failed), rather than assuming ownership
    // transferred cleanly, since another thread's fresh (non-waiting) Enter
    // could have raced in first.
    static_cast<void>(
        kernel::wait_for_single_object(event, xbox_infinite_timeout(), context.thread_id));
  }
}

// RtlTryEnterCriticalSection (ordinal 0x141)
// Guest ABI: r3 = X_RTL_CRITICAL_SECTION guest pointer -> r3 = BOOLEAN
// (nonzero = acquired).
bool rtl_try_enter_critical_section_export(kernel::KernelProcess& process,
                                           ExportCallContext& context) {
  const auto cs = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  if (cs == 0u) {
    context.cpu.gpr[3] = 0u;
    return true;
  }
  const auto self = owning_thread_marker(context);

  std::lock_guard<std::mutex> lock(process.critical_section_mutex());
  const auto owner = context.memory.read32_be(cs + kOwningThreadOffset);
  if (owner == 0u) {
    context.memory.write32_be(cs + kOwningThreadOffset, self);
    context.memory.write32_be(cs + kRecursionCountOffset, 1u);
    context.memory.write32_be(cs + kLockCountOffset, 0u);
    context.cpu.gpr[3] = 1u;
  } else if (owner == self) {
    context.memory.write32_be(cs + kRecursionCountOffset,
                              context.memory.read32_be(cs + kRecursionCountOffset) + 1u);
    context.memory.write32_be(cs + kLockCountOffset,
                              context.memory.read32_be(cs + kLockCountOffset) + 1u);
    context.cpu.gpr[3] = 1u;
  } else {
    context.cpu.gpr[3] = 0u;
  }
  return true;
}

// RtlLeaveCriticalSection (ordinal 0x130)
// Guest ABI: r3 = X_RTL_CRITICAL_SECTION guest pointer -> void.
bool rtl_leave_critical_section_export(kernel::KernelProcess& process,
                                       ExportCallContext& context) {
  const auto cs = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  if (cs == 0u) return true;

  bool should_wake = false;
  {
    std::lock_guard<std::mutex> lock(process.critical_section_mutex());
    const auto recursion = context.memory.read32_be(cs + kRecursionCountOffset);
    if (recursion == 0u) return true;  // not held - nothing to do, matches real hardware's assert-only guard
    const auto new_recursion = recursion - 1u;
    context.memory.write32_be(cs + kRecursionCountOffset, new_recursion);
    if (new_recursion != 0u) {
      // Still held (nested) - just drop one layer of the waiter-count-ish
      // lock_count bookkeeping.
      context.memory.write32_be(
          cs + kLockCountOffset, context.memory.read32_be(cs + kLockCountOffset) - 1u);
      return true;
    }
    context.memory.write32_be(cs + kOwningThreadOffset, 0u);
    const auto new_lock_count =
        static_cast<std::int32_t>(context.memory.read32_be(cs + kLockCountOffset)) - 1;
    context.memory.write32_be(cs + kLockCountOffset, static_cast<std::uint32_t>(new_lock_count));
    should_wake = new_lock_count != -1;  // -1 means no waiters were registered
  }
  if (should_wake) {
    if (auto event = resolve_wait_event(process, context, cs, "RtlLeaveCriticalSection")) {
      event->set();
    }
  }
  return true;
}

namespace {
struct RtlCriticalSectionExportSpec {
  std::uint32_t ordinal;
  const char* name;
  bool (*handler)(kernel::KernelProcess&, ExportCallContext&);
};

// Ordinals verified against the xenia-project/xenia xboxkrnl export table
// (xboxkrnl_table.inc), not guessed - see
// xboxkrnl_rtl_critical_section_exports.hpp.
const RtlCriticalSectionExportSpec kRtlCriticalSectionExports[] = {
    {0x125u, "RtlEnterCriticalSection", &rtl_enter_critical_section_export},
    {0x12Eu, "RtlInitializeCriticalSection", &rtl_initialize_critical_section_export},
    {0x12Fu, "RtlInitializeCriticalSectionAndSpinCount",
     &rtl_initialize_critical_section_and_spin_count_export},
    {0x130u, "RtlLeaveCriticalSection", &rtl_leave_critical_section_export},
    {0x141u, "RtlTryEnterCriticalSection", &rtl_try_enter_critical_section_export},
};
}  // namespace

bool register_xboxkrnl_rtl_critical_section_exports(core::ExportRegistry& registry,
                                                     kernel::KernelProcess& process) {
  for (const auto& spec : kRtlCriticalSectionExports) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = spec.name;
    descriptor.ordinal = spec.ordinal;
    descriptor.requirement = core::ExportRequirement::Required;
    descriptor.handler = [&process, fn = spec.handler](ExportCallContext& ctx) {
      return fn(process, ctx);
    };

    if (!registry.register_export(std::move(descriptor))) {
      return false;
    }
  }

  return true;
}

}  // namespace xenon::xbox
