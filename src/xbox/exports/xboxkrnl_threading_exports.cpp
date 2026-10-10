#include "xenon/xbox/xboxkrnl_threading_exports.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/xbox_io.hpp"
#include "xenon/xbox/xboxkrnl_ke_sync_exports.hpp"
#include "xenon/xbox/xex_dispatcher_header.hpp"

namespace xenon::xbox {
namespace {

using core::ExportCallContext;
namespace status = xenon::kernel::xbox::status;

constexpr std::uint32_t kStatusThreadIsTerminating = 0xC000004Bu;

// X_ERWLOCK field offsets (0x38 bytes).
constexpr std::uint32_t kLockCount = 0x00u;
constexpr std::uint32_t kWritersWaiting = 0x04u;
constexpr std::uint32_t kReadersWaiting = 0x08u;
constexpr std::uint32_t kReadersEntry = 0x0Cu;
constexpr std::uint32_t kWriterEvent = 0x10u;
constexpr std::uint32_t kReaderSemaphore = 0x20u;
constexpr std::uint32_t kSpinLock = 0x34u;

// Runs one of the raw-guest-object Ke* handlers on a private register set so the
// caller's registers are untouched, and returns its r3.
using KeHandler = bool (*)(kernel::KernelProcess&, ExportCallContext&);
std::uint64_t call_ke(kernel::KernelProcess& process, ExportCallContext& context, KeHandler handler,
                      std::uint64_t r3, std::uint64_t r4 = 0, std::uint64_t r5 = 0,
                      std::uint64_t r6 = 0, std::uint64_t r7 = 0) {
  cpu::CpuState local = context.cpu;
  local.gpr[3] = r3;
  local.gpr[4] = r4;
  local.gpr[5] = r5;
  local.gpr[6] = r6;
  local.gpr[7] = r7;
  ExportCallContext inner{local, context.memory, context.call_address, context.thread_id};
  static_cast<void>(handler(process, inner));
  return local.gpr[3];
}

std::int32_t read_i32(cpu::MemoryPort& memory, cpu::GuestAddress address) {
  return static_cast<std::int32_t>(memory.read32_be(address));
}
void write_i32(cpu::MemoryPort& memory, cpu::GuestAddress address, std::int32_t value) {
  memory.write32_be(address, static_cast<std::uint32_t>(value));
}

kernel::KernelThread* thread_for_kthread(kernel::KernelProcess& process, cpu::GuestAddress address) {
  for (const auto& thread : process.thread_manager().enumerate_threads()) {
    if (thread->guest_kthread_address() == address) return thread.get();
  }
  return nullptr;
}

}  // namespace

// ---- Executive read/write lock ----------------------------------------------
// The lock's own fields are protected by the process-wide critical-section mutex
// in place of the guest spin lock (the same substitution the Rtl critical
// sections make); the mutex is never held across a wait.

bool ex_initialize_read_write_lock_export(kernel::KernelProcess&, ExportCallContext& context) {
  const auto lock = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  auto& memory = context.memory;
  write_i32(memory, lock + kLockCount, -1);
  memory.write32_be(lock + kWritersWaiting, 0u);
  memory.write32_be(lock + kReadersWaiting, 0u);
  memory.write32_be(lock + kReadersEntry, 0u);
  // A synchronization event (type 1) and a semaphore with a maximal limit, as
  // KeInitializeEvent(event, 1, 0) / KeInitializeSemaphore(sem, 0, 0x7FFFFFFF) do.
  initialize_dispatch_header(memory, lock + kWriterEvent, DispatchObjectType::EventSynchronization, 0u);
  initialize_dispatch_header(memory, lock + kReaderSemaphore, DispatchObjectType::Semaphore, 0u,
                             0x7FFFFFFF);
  memory.write32_be(lock + kSpinLock, 0u);
  return true;
}

bool ex_acquire_read_write_lock_exclusive_export(kernel::KernelProcess& process,
                                                 ExportCallContext& context) {
  const auto lock = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  auto& memory = context.memory;
  {
    std::scoped_lock guard(process.critical_section_mutex());
    const auto count = read_i32(memory, lock + kLockCount) + 1;
    write_i32(memory, lock + kLockCount, count);
    if (count == 0) return true;  // uncontended
    memory.write32_be(lock + kWritersWaiting, memory.read32_be(lock + kWritersWaiting) + 1u);
  }
  // Wait for the previous holder(s) to hand the lock over via the writer event.
  static_cast<void>(call_ke(process, context, &ke_wait_for_single_object_export, lock + kWriterEvent, 7u));
  return true;
}

bool ex_acquire_read_write_lock_shared_export(kernel::KernelProcess& process,
                                              ExportCallContext& context) {
  const auto lock = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  auto& memory = context.memory;
  {
    std::scoped_lock guard(process.critical_section_mutex());
    const auto count = read_i32(memory, lock + kLockCount) + 1;
    write_i32(memory, lock + kLockCount, count);
    if (count == 0 || (memory.read32_be(lock + kReadersEntry) != 0u &&
                       memory.read32_be(lock + kWritersWaiting) == 0u)) {
      memory.write32_be(lock + kReadersEntry, memory.read32_be(lock + kReadersEntry) + 1u);
      return true;
    }
    memory.write32_be(lock + kReadersWaiting, memory.read32_be(lock + kReadersWaiting) + 1u);
  }
  static_cast<void>(call_ke(process, context, &ke_wait_for_single_object_export, lock + kReaderSemaphore, 7u));
  return true;
}

bool ex_try_to_acquire_read_write_lock_exclusive_export(kernel::KernelProcess& process,
                                                        ExportCallContext& context) {
  const auto lock = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  std::scoped_lock guard(process.critical_section_mutex());
  if (read_i32(context.memory, lock + kLockCount) < 0) {
    write_i32(context.memory, lock + kLockCount, 0);
    context.cpu.gpr[3] = 1u;
  } else {
    context.cpu.gpr[3] = 0u;
  }
  return true;
}

bool ex_try_to_acquire_read_write_lock_shared_export(kernel::KernelProcess& process,
                                                     ExportCallContext& context) {
  const auto lock = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  auto& memory = context.memory;
  std::scoped_lock guard(process.critical_section_mutex());
  if (read_i32(memory, lock + kLockCount) < 0 || (memory.read32_be(lock + kReadersEntry) != 0u &&
                                                  memory.read32_be(lock + kWritersWaiting) == 0u)) {
    write_i32(memory, lock + kLockCount, read_i32(memory, lock + kLockCount) + 1);
    memory.write32_be(lock + kReadersEntry, memory.read32_be(lock + kReadersEntry) + 1u);
    context.cpu.gpr[3] = 1u;
  } else {
    context.cpu.gpr[3] = 0u;
  }
  return true;
}

bool ex_release_read_write_lock_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto lock = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  auto& memory = context.memory;
  std::uint32_t wake_readers = 0;
  bool wake_writer = false;
  {
    std::scoped_lock guard(process.critical_section_mutex());
    const auto count = read_i32(memory, lock + kLockCount) - 1;
    write_i32(memory, lock + kLockCount, count);
    if (count < 0) {  // now unlocked
      memory.write32_be(lock + kReadersEntry, 0u);
      return true;
    }
    // The lock is still wanted (count >= 0). readers_entry == 0 means the releasing
    // holder was a writer; otherwise it was one of the current readers.
    //
    // NOTE: the reference implementations (xenia and rexglue) fall through to
    // "--readers_entry" after a writer releases with only *writers* waiting, which
    // wraps the zero count to 0xFFFFFFFF and never wakes the next writer - a
    // deadlock under any writer-vs-writer contention. Xenon uses the self-
    // consistent form: a releasing writer hands the lock to all waiting readers, or
    // failing that to the next waiting writer; the last releasing reader wakes the
    // next waiting writer (writers are preferred over readers that queued behind
    // them).
    if (memory.read32_be(lock + kReadersEntry) == 0u) {
      const auto waiting = memory.read32_be(lock + kReadersWaiting);
      if (waiting != 0u) {  // hand the lock to every waiting reader at once
        memory.write32_be(lock + kReadersWaiting, 0u);
        memory.write32_be(lock + kReadersEntry, waiting);
        wake_readers = waiting;
      } else {  // only writers are waiting: wake the next one
        memory.write32_be(lock + kWritersWaiting, memory.read32_be(lock + kWritersWaiting) - 1u);
        wake_writer = true;
      }
    } else {
      const auto entry = memory.read32_be(lock + kReadersEntry) - 1u;
      memory.write32_be(lock + kReadersEntry, entry);
      if (entry != 0u) return true;  // other readers still hold it
      memory.write32_be(lock + kWritersWaiting, memory.read32_be(lock + kWritersWaiting) - 1u);
      wake_writer = true;
    }
  }
  if (wake_readers != 0u) {
    static_cast<void>(call_ke(process, context, &ke_release_semaphore_export, lock + kReaderSemaphore,
                              1u, wake_readers, 0u));
  } else if (wake_writer) {
    static_cast<void>(call_ke(process, context, &ke_set_event_export, lock + kWriterEvent, 1u, 0u));
  }
  return true;
}

// ---- Events, threads, misc ---------------------------------------------------

bool ke_pulse_event_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto header = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  std::string error;
  auto object = resolve_dispatcher_object(process, context.memory, header, &error);
  if (!object || object->type() != kernel::ObjectType::Event) {
    context.cpu.gpr[3] = 0u;
    return true;
  }
  auto& event = static_cast<kernel::KernelEvent&>(*object);
  const std::uint32_t previous = event.signaled() ? 1u : 0u;
  // Release the current waiters, then reset - PulseEvent's documented semantics.
  event.set();
  event.reset();
  context.cpu.gpr[3] = previous;
  return true;
}

bool nt_query_event_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle = static_cast<kernel::Handle>(context.cpu.gpr[3]);
  const auto out = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  kernel::HandleView view{};
  const auto lookup = process.handle_table().lookup(handle, view);
  if (lookup != kernel::KernelIoCode::Success) {
    context.cpu.gpr[3] = status::InvalidHandle;
    return true;
  }
  if (view.object->type() != kernel::ObjectType::Event) {
    context.cpu.gpr[3] = status::ObjectTypeMismatch;
    return true;
  }
  if (out == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  const auto& event = static_cast<const kernel::KernelEvent&>(*view.object);
  // EVENT_BASIC_INFORMATION: EventType (0 notification, 1 synchronization), state.
  context.memory.write32_be(out + 0u, event.manual_reset() ? 0u : 1u);
  context.memory.write32_be(out + 4u, event.signaled() ? 1u : 0u);
  context.cpu.gpr[3] = status::Success;
  return true;
}

bool ke_suspend_thread_export(kernel::KernelProcess& process, ExportCallContext& context) {
  auto* thread = thread_for_kthread(process, static_cast<cpu::GuestAddress>(context.cpu.gpr[3]));
  std::uint32_t previous = 0u;
  if (thread != nullptr) {
    previous = thread->suspend_count();
    static_cast<void>(thread->suspend());
  }
  context.cpu.gpr[3] = previous;
  return true;
}

bool nt_suspend_thread_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto handle = static_cast<kernel::Handle>(context.cpu.gpr[3]);
  const auto previous_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  kernel::HandleView view{};
  const auto lookup = process.handle_table().lookup(handle, view);
  if (lookup != kernel::KernelIoCode::Success) {
    context.cpu.gpr[3] = status::InvalidHandle;
    return true;
  }
  if (view.object->type() != kernel::ObjectType::Thread) {
    context.cpu.gpr[3] = status::ObjectTypeMismatch;
    return true;
  }
  auto& thread = static_cast<kernel::KernelThread&>(*view.object);
  if (thread.is_terminated()) {
    context.cpu.gpr[3] = kStatusThreadIsTerminating;
    return true;
  }
  const auto previous = thread.suspend_count();
  static_cast<void>(thread.suspend());
  if (previous_ptr != 0u) context.memory.write32_be(previous_ptr, previous);
  context.cpu.gpr[3] = status::Success;
  return true;
}

bool ke_set_disable_boost_thread_export(kernel::KernelProcess& process, ExportCallContext& context) {
  auto* thread = thread_for_kthread(process, static_cast<cpu::GuestAddress>(context.cpu.gpr[3]));
  const bool disabled = (context.cpu.gpr[4] & 0xFFu) != 0u;
  context.cpu.gpr[3] = thread != nullptr && thread->exchange_boost_disabled(disabled) ? 1u : 0u;
  return true;
}

bool fsc_get_cache_element_count_export(kernel::KernelProcess& process, ExportCallContext& context) {
  context.cpu.gpr[3] = process.fsc_cache_element_count().load();
  return true;
}

bool fsc_set_cache_element_count_export(kernel::KernelProcess& process, ExportCallContext& context) {
  // (r3 is always 0; r4 is the element count - commonly 256.)
  process.fsc_cache_element_count().store(static_cast<std::uint32_t>(context.cpu.gpr[4]));
  context.cpu.gpr[3] = status::Success;
  return true;
}

// ---- Interlocked singly-linked list ------------------------------------------
// SLIST_HEADER is 8 bytes: { PSINGLE_LIST_ENTRY Next; USHORT Depth; USHORT
// Sequence; } and a SINGLE_LIST_ENTRY is one pointer. The whole header is one
// big-endian 64-bit word (next in the high half), updated atomically.

namespace {

constexpr std::uint64_t pack(std::uint32_t next, std::uint16_t depth, std::uint16_t sequence) {
  return (static_cast<std::uint64_t>(next) << 32) | (static_cast<std::uint64_t>(depth) << 16) |
         sequence;
}
constexpr std::uint32_t next_of(std::uint64_t header) { return static_cast<std::uint32_t>(header >> 32); }
constexpr std::uint16_t depth_of(std::uint64_t header) { return static_cast<std::uint16_t>(header >> 16); }
constexpr std::uint16_t sequence_of(std::uint64_t header) { return static_cast<std::uint16_t>(header); }

bool interlocked_push_entry_slist(ExportCallContext& context) {
  const auto list = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto entry = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  for (;;) {
    // Link the entry to the head we are about to displace BEFORE taking the
    // reservation: a store to guest memory between load-reserve and
    // store-conditional cancels the reservation when the entry happens to share
    // a reservation granule with the list header (entries are often allocated
    // right next to it), which would make the loop spin forever. The header is
    // then re-read under the reservation and, if it no longer matches the head
    // the entry was linked to, the attempt is retried.
    const auto observed = context.memory.read64_be(list);
    context.memory.write32_be(entry, next_of(observed));  // entry->Next = old head
    std::uint64_t old_header = 0;
    const auto token = context.memory.reserve64(list, old_header);
    if (next_of(old_header) != next_of(observed)) {
      context.memory.cancel_reservation(token);
      continue;
    }
    const auto new_header = pack(entry, static_cast<std::uint16_t>(depth_of(old_header) + 1u),
                                 static_cast<std::uint16_t>(sequence_of(old_header) + 1u));
    if (context.memory.store_conditional64(list, token, new_header)) {
      context.cpu.gpr[3] = next_of(old_header);  // the previous first entry
      return true;
    }
  }
}

bool interlocked_pop_entry_slist(ExportCallContext& context) {
  const auto list = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  for (;;) {
    std::uint64_t old_header = 0;
    const auto token = context.memory.reserve64(list, old_header);
    const auto head = next_of(old_header);
    if (head == 0u) {
      context.memory.cancel_reservation(token);
      context.cpu.gpr[3] = 0u;
      return true;
    }
    const auto next = context.memory.read32_be(head);
    const auto new_header = pack(next, static_cast<std::uint16_t>(depth_of(old_header) - 1u),
                                 sequence_of(old_header));
    if (context.memory.store_conditional64(list, token, new_header)) {
      context.cpu.gpr[3] = head;
      return true;
    }
  }
}

bool interlocked_flush_slist(ExportCallContext& context) {
  const auto list = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  for (;;) {
    std::uint64_t old_header = 0;
    const auto token = context.memory.reserve64(list, old_header);
    if (context.memory.store_conditional64(list, token, 0u)) {
      context.cpu.gpr[3] = next_of(old_header);  // the whole chain, detached
      return true;
    }
  }
}

// NtYieldExecution: give up the rest of the time slice. STATUS_SUCCESS means
// another thread was made runnable; a host yield reports success.
bool nt_yield_execution(ExportCallContext& context) {
  std::this_thread::yield();
  context.cpu.gpr[3] = status::Success;
  return true;
}

// KfRaiseIrql: r3 = new IRQL -> r3 = previous IRQL. Xenon does not model
// interrupt-level preemption (see xboxkrnl_ke_irql_exports.cpp), so the previous
// level is always PASSIVE_LEVEL, consistent with KeRaiseIrqlToDpcLevel.
bool kf_raise_irql(ExportCallContext& context) {
  context.cpu.gpr[3] = 0u;
  return true;
}

// KeEnableFpuExceptions: r3 = enable -> void. Titles ask for FPU exceptions to be
// delivered; Xenon runs guest floating point on the host with the exception
// masks the recompiled code was verified under, so no trap can be delivered.
bool ke_enable_fpu_exceptions(ExportCallContext&) { return true; }

}  // namespace

bool register_xboxkrnl_threading_exports(core::ExportRegistry& registry) {
  struct Spec {
    std::uint32_t ordinal;
    const char* name;
    core::ExportHandler handler;
  };
  const Spec specs[] = {
      {0x2Du, "InterlockedPushEntrySList", &interlocked_push_entry_slist},
      {0x2Cu, "InterlockedPopEntrySList", &interlocked_pop_entry_slist},
      {0x2Bu, "InterlockedFlushSList", &interlocked_flush_slist},
      {0x101u, "NtYieldExecution", &nt_yield_execution},
      {0xB2u, "KfRaiseIrql", &kf_raise_irql},
      {0x5Du, "KeEnableFpuExceptions", &ke_enable_fpu_exceptions},
  };
  for (const auto& spec : specs) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = spec.name;
    descriptor.ordinal = spec.ordinal;
    descriptor.requirement = core::ExportRequirement::Required;
    descriptor.handler = spec.handler;
    if (!registry.register_export(std::move(descriptor))) return false;
  }
  return true;
}

}  // namespace xenon::xbox
