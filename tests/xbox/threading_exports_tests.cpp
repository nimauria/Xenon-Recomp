// Executive read/write locks, interlocked singly-linked lists, KePulseEvent,
// NtQueryEvent, thread suspension, KeSetDisableBoostThread, the FSC cache count
// and the small process-free threading exports (NtYieldExecution, KfRaiseIrql,
// KeEnableFpuExceptions).
//
// Exports are driven through core::ExportRegistry::invoke() with real guest
// memory and a real KernelProcess. Lock and list behaviour is verified with real
// concurrent host threads, since that is where these primitives earn their keep.

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xboxkrnl_ke_sync_exports.hpp"
#include "xenon/xbox/xboxkrnl_threading_exports.hpp"
#include "xenon/xbox/xex_dispatcher_header.hpp"

using namespace xenon;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kStatusSuccess = 0u;
constexpr std::uint32_t kStatusInvalidHandle = 0xC0000008u;
constexpr std::uint32_t kStatusObjectTypeMismatch = 0xC0000024u;
constexpr std::uint32_t kStatusThreadIsTerminating = 0xC000004Bu;

constexpr std::uint32_t kOrdExInitializeRwLock = 0x11u;
constexpr std::uint32_t kOrdExAcquireExclusive = 0x07u;
constexpr std::uint32_t kOrdExAcquireShared = 0x08u;
constexpr std::uint32_t kOrdExTryExclusive = 0x2DDu;
constexpr std::uint32_t kOrdExTryShared = 0x2DEu;
constexpr std::uint32_t kOrdExRelease = 0x16u;
constexpr std::uint32_t kOrdKePulseEvent = 0x7Fu;
constexpr std::uint32_t kOrdNtQueryEvent = 0xE6u;
constexpr std::uint32_t kOrdKeSuspendThread = 0xA9u;
constexpr std::uint32_t kOrdNtSuspendThread = 0xFCu;
constexpr std::uint32_t kOrdKeSetDisableBoost = 0x9Cu;
constexpr std::uint32_t kOrdFscGet = 0x20u;
constexpr std::uint32_t kOrdFscSet = 0x21u;
constexpr std::uint32_t kOrdSListFlush = 0x2Bu;
constexpr std::uint32_t kOrdSListPop = 0x2Cu;
constexpr std::uint32_t kOrdSListPush = 0x2Du;
constexpr std::uint32_t kOrdYield = 0x101u;
constexpr std::uint32_t kOrdKfRaiseIrql = 0xB2u;
constexpr std::uint32_t kOrdKeEnableFpuExceptions = 0x5Du;

struct Fixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  std::shared_ptr<kernel::KernelMemory> kernel_memory;
  std::unique_ptr<kernel::KernelProcess> process;
  core::ExportRegistry registry;
  memory::GuestAddress arena{};
  std::atomic<std::uint32_t> arena_used{0};
  static constexpr std::uint32_t kArena = 0x40000u;

  using Handler = bool (*)(kernel::KernelProcess&, core::ExportCallContext&);
  void add(std::uint32_t ordinal, const char* name, Handler handler) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = name;
    descriptor.ordinal = ordinal;
    kernel::KernelProcess* raw = process.get();
    descriptor.handler = [raw, handler](core::ExportCallContext& ctx) { return handler(*raw, ctx); };
    const bool ok = registry.register_export(std::move(descriptor));
    assert(ok);
    static_cast<void>(ok);
  }

  Fixture() {
    address_space = std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    const bool ok = address_space->initialize();
    assert(ok);
    kernel_memory = std::make_shared<kernel::KernelMemory>(address_space);
    process = std::make_unique<kernel::KernelProcess>(kernel_memory);
    const bool a_ok = address_space->allocate(kArena, 0x1000u, memory::kReadWrite, true, arena);
    assert(a_ok);
    static_cast<void>(ok);
    static_cast<void>(a_ok);
    add(kOrdExInitializeRwLock, "ExInitializeReadWriteLock", &xbox::ex_initialize_read_write_lock_export);
    add(kOrdExAcquireExclusive, "ExAcquireReadWriteLockExclusive",
        &xbox::ex_acquire_read_write_lock_exclusive_export);
    add(kOrdExAcquireShared, "ExAcquireReadWriteLockShared", &xbox::ex_acquire_read_write_lock_shared_export);
    add(kOrdExTryExclusive, "ExTryToAcquireReadWriteLockExclusive",
        &xbox::ex_try_to_acquire_read_write_lock_exclusive_export);
    add(kOrdExTryShared, "ExTryToAcquireReadWriteLockShared",
        &xbox::ex_try_to_acquire_read_write_lock_shared_export);
    add(kOrdExRelease, "ExReleaseReadWriteLock", &xbox::ex_release_read_write_lock_export);
    add(kOrdKePulseEvent, "KePulseEvent", &xbox::ke_pulse_event_export);
    add(kOrdNtQueryEvent, "NtQueryEvent", &xbox::nt_query_event_export);
    add(kOrdKeSuspendThread, "KeSuspendThread", &xbox::ke_suspend_thread_export);
    add(kOrdNtSuspendThread, "NtSuspendThread", &xbox::nt_suspend_thread_export);
    add(kOrdKeSetDisableBoost, "KeSetDisableBoostThread", &xbox::ke_set_disable_boost_thread_export);
    add(kOrdFscGet, "FscGetCacheElementCount", &xbox::fsc_get_cache_element_count_export);
    add(kOrdFscSet, "FscSetCacheElementCount", &xbox::fsc_set_cache_element_count_export);
    const bool reg = xbox::register_xboxkrnl_threading_exports(registry);
    assert(reg);
    static_cast<void>(reg);
  }

  memory::GuestAddress alloc(std::uint32_t bytes) {
    const auto aligned = (bytes + 15u) & ~15u;
    const auto offset = arena_used.fetch_add(aligned);
    assert(offset + aligned <= kArena);
    return arena + offset;
  }

  std::uint64_t call(std::uint32_t ordinal, std::uint64_t r3 = 0, std::uint64_t r4 = 0,
                     std::uint64_t r5 = 0, std::uint32_t thread_id = 0) {
    cpu::CpuState cpu{};
    cpu.gpr[3] = r3;
    cpu.gpr[4] = r4;
    cpu.gpr[5] = r5;
    core::ExportCallContext ctx{cpu, *address_space, 0, thread_id};
    const auto result = registry.invoke("xboxkrnl", ordinal, ctx);
    assert(result.handled && result.success);
    return cpu.gpr[3];
  }
};

// ---- ERWLOCK ------------------------------------------------------------------
std::int32_t lock_count(Fixture& f, memory::GuestAddress lock) {
  return static_cast<std::int32_t>(f.address_space->read32_be(lock));
}

void test_rwlock_initialize_and_uncontended() {
  Fixture f;
  const auto lock = f.alloc(0x40);
  f.address_space->fill_bytes(lock, 0x40, 0xEE);
  f.call(kOrdExInitializeRwLock, lock);
  assert(lock_count(f, lock) == -1);
  assert(f.address_space->read32_be(lock + 4u) == 0u && f.address_space->read32_be(lock + 8u) == 0u &&
         f.address_space->read32_be(lock + 12u) == 0u);
  assert(f.address_space->read8(lock + 0x10u) == 1u && "writer event is a synchronization event");
  assert(f.address_space->read8(lock + 0x20u) == 5u && "reader semaphore");
  assert(f.address_space->read32_be(lock + 0x30u) == 0x7FFFFFFFu && "semaphore limit");

  f.call(kOrdExAcquireExclusive, lock);
  assert(lock_count(f, lock) == 0);
  f.call(kOrdExRelease, lock);
  assert(lock_count(f, lock) == -1 && f.address_space->read32_be(lock + 12u) == 0u);

  f.call(kOrdExAcquireShared, lock);
  f.call(kOrdExAcquireShared, lock);
  assert(lock_count(f, lock) == 1 && f.address_space->read32_be(lock + 12u) == 2u && "two readers");
  f.call(kOrdExRelease, lock);
  f.call(kOrdExRelease, lock);
  assert(lock_count(f, lock) == -1);
}

void test_rwlock_try_variants() {
  Fixture f;
  const auto lock = f.alloc(0x40);
  f.call(kOrdExInitializeRwLock, lock);
  assert(f.call(kOrdExTryExclusive, lock) == 1u && "free lock: exclusive succeeds");
  assert(f.call(kOrdExTryExclusive, lock) == 0u && "held: exclusive fails");
  assert(f.call(kOrdExTryShared, lock) == 0u && "held exclusively: shared fails");
  f.call(kOrdExRelease, lock);
  assert(f.call(kOrdExTryShared, lock) == 1u && f.call(kOrdExTryShared, lock) == 1u);
  assert(f.call(kOrdExTryExclusive, lock) == 0u && "readers hold it: exclusive fails");
  f.call(kOrdExRelease, lock);
  f.call(kOrdExRelease, lock);
  assert(lock_count(f, lock) == -1);
}

// A writer blocks while a reader holds the lock and runs once it is released.
void test_rwlock_writer_waits_for_reader() {
  Fixture f;
  const auto lock = f.alloc(0x40);
  f.call(kOrdExInitializeRwLock, lock);
  f.call(kOrdExAcquireShared, lock, 0, 0, 1);

  std::atomic<bool> writer_in{false};
  std::thread writer([&] {
    f.call(kOrdExAcquireExclusive, lock, 0, 0, 2);
    writer_in = true;
    f.call(kOrdExRelease, lock, 0, 0, 2);
  });
  std::this_thread::sleep_for(100ms);
  assert(!writer_in.load() && "the writer must wait for the reader");
  assert(f.address_space->read32_be(lock + 4u) == 1u && "one writer is waiting");

  f.call(kOrdExRelease, lock, 0, 0, 1);
  writer.join();
  assert(writer_in.load());
  assert(lock_count(f, lock) == -1);
}

// A writer that releases while another writer is waiting hands the lock straight
// to it. (The reference implementations lose this wakeup and deadlock.)
void test_rwlock_writer_hands_off_to_writer() {
  Fixture f;
  const auto lock = f.alloc(0x40);
  f.call(kOrdExInitializeRwLock, lock);
  f.call(kOrdExAcquireExclusive, lock, 0, 0, 1);

  std::atomic<bool> second_in{false};
  std::thread second([&] {
    f.call(kOrdExAcquireExclusive, lock, 0, 0, 2);
    second_in = true;
    f.call(kOrdExRelease, lock, 0, 0, 2);
  });
  std::this_thread::sleep_for(100ms);
  assert(!second_in.load() && f.address_space->read32_be(lock + 4u) == 1u);
  f.call(kOrdExRelease, lock, 0, 0, 1);
  second.join();
  assert(second_in.load() && "the waiting writer was woken by the releasing writer");
  assert(lock_count(f, lock) == -1);
}

// Readers that queue behind a writer are all released together when it finishes.
void test_rwlock_readers_wait_for_writer_and_are_released_together() {
  Fixture f;
  const auto lock = f.alloc(0x40);
  f.call(kOrdExInitializeRwLock, lock);
  f.call(kOrdExAcquireExclusive, lock, 0, 0, 1);

  std::atomic<int> readers_in{0};
  std::vector<std::thread> readers;
  for (std::uint32_t i = 0; i < 3; ++i) {
    readers.emplace_back([&, i] {
      f.call(kOrdExAcquireShared, lock, 0, 0, 10 + i);
      ++readers_in;
    });
  }
  std::this_thread::sleep_for(150ms);
  assert(readers_in.load() == 0 && "readers wait while the writer holds the lock");
  assert(f.address_space->read32_be(lock + 8u) == 3u && "three readers are waiting");

  f.call(kOrdExRelease, lock, 0, 0, 1);
  for (auto& t : readers) t.join();
  assert(readers_in.load() == 3 && "every waiting reader is admitted");
  assert(f.address_space->read32_be(lock + 12u) == 3u && "readers-entry counts them");
  for (int i = 0; i < 3; ++i) f.call(kOrdExRelease, lock);
  assert(lock_count(f, lock) == -1);
}

// Mutual exclusion under contention: exclusive holders never overlap and shared
// holders never coexist with a writer.
void test_rwlock_stress() {
  Fixture f;
  const auto lock = f.alloc(0x40);
  f.call(kOrdExInitializeRwLock, lock);
  std::atomic<int> writers_active{0};
  std::atomic<int> readers_active{0};
  std::atomic<bool> violation{false};
  std::atomic<int> operations{0};
  std::vector<std::thread> threads;
  for (std::uint32_t t = 0; t < 6; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 200; ++i) {
        if ((i + static_cast<int>(t)) % 4 == 0) {
          f.call(kOrdExAcquireExclusive, lock, 0, 0, 100 + t);
          if (writers_active.fetch_add(1) != 0 || readers_active.load() != 0) violation = true;
          std::this_thread::yield();
          writers_active.fetch_sub(1);
          f.call(kOrdExRelease, lock, 0, 0, 100 + t);
        } else {
          f.call(kOrdExAcquireShared, lock, 0, 0, 100 + t);
          readers_active.fetch_add(1);
          if (writers_active.load() != 0) violation = true;
          std::this_thread::yield();
          readers_active.fetch_sub(1);
          f.call(kOrdExRelease, lock, 0, 0, 100 + t);
        }
        ++operations;
      }
    });
  }
  for (auto& t : threads) t.join();
  assert(!violation.load() && "the lock's exclusion guarantees held");
  assert(operations.load() == 6 * 200);
  assert(lock_count(f, lock) == -1 && "the lock ends free");
}

// ---- SList --------------------------------------------------------------------
struct ListView {
  std::uint32_t next, depth, sequence;
};
ListView read_list(Fixture& f, memory::GuestAddress list) {
  return {f.address_space->read32_be(list), f.address_space->read16_be(list + 4u),
          f.address_space->read16_be(list + 6u)};
}

void test_slist_push_pop_flush() {
  Fixture f;
  const auto list = f.alloc(8);
  f.address_space->fill_bytes(list, 8, 0);
  const auto a = f.alloc(8), b = f.alloc(8), c = f.alloc(8);

  assert(f.call(kOrdSListPop, list) == 0u && "popping an empty list");
  assert(f.call(kOrdSListPush, list, a) == 0u && "push returns the previous first entry (none)");
  assert(f.call(kOrdSListPush, list, b) == a);
  assert(f.call(kOrdSListPush, list, c) == b);
  auto view = read_list(f, list);
  assert(view.next == c && view.depth == 3u && view.sequence == 3u);
  assert(f.address_space->read32_be(c) == b && f.address_space->read32_be(b) == a &&
         f.address_space->read32_be(a) == 0u && "entries link LIFO");

  assert(f.call(kOrdSListPop, list) == c);
  view = read_list(f, list);
  assert(view.next == b && view.depth == 2u && view.sequence == 3u && "pop lowers depth, keeps sequence");
  assert(f.call(kOrdSListPop, list) == b && f.call(kOrdSListPop, list) == a);
  assert(f.call(kOrdSListPop, list) == 0u);
  view = read_list(f, list);
  assert(view.next == 0u && view.depth == 0u);

  f.call(kOrdSListPush, list, a);
  f.call(kOrdSListPush, list, b);
  assert(f.call(kOrdSListFlush, list) == b && "flush returns the whole chain");
  view = read_list(f, list);
  assert(view.next == 0u && view.depth == 0u && view.sequence == 0u);
  assert(f.address_space->read32_be(b) == a && "the detached chain stays linked");
  assert(f.call(kOrdSListFlush, list) == 0u);
}

// Lock-free correctness under concurrency: every entry pushed by several threads
// is popped exactly once.
void test_slist_concurrent() {
  Fixture f;
  const auto list = f.alloc(8);
  f.address_space->fill_bytes(list, 8, 0);
  constexpr std::uint32_t kPerThread = 300;
  constexpr std::uint32_t kThreads = 4;
  std::vector<memory::GuestAddress> entries;
  for (std::uint32_t i = 0; i < kPerThread * kThreads; ++i) entries.push_back(f.alloc(8));

  std::vector<std::thread> pushers;
  for (std::uint32_t t = 0; t < kThreads; ++t) {
    pushers.emplace_back([&, t] {
      for (std::uint32_t i = 0; i < kPerThread; ++i) f.call(kOrdSListPush, list, entries[t * kPerThread + i]);
    });
  }
  std::vector<std::vector<std::uint32_t>> popped(kThreads);
  std::atomic<std::uint32_t> total{0};
  std::vector<std::thread> poppers;
  for (std::uint32_t t = 0; t < kThreads; ++t) {
    poppers.emplace_back([&, t] {
      while (total.load() < kPerThread * kThreads) {
        const auto entry = static_cast<std::uint32_t>(f.call(kOrdSListPop, list));
        if (entry != 0u) {
          popped[t].push_back(entry);
          ++total;
        }
      }
    });
  }
  for (auto& t : pushers) t.join();
  for (auto& t : poppers) t.join();

  std::vector<std::uint32_t> all;
  for (auto& v : popped) all.insert(all.end(), v.begin(), v.end());
  assert(all.size() == entries.size());
  std::sort(all.begin(), all.end());
  auto expected = entries;
  std::sort(expected.begin(), expected.end());
  assert(all == expected && "each entry is popped exactly once");
  assert(read_list(f, list).next == 0u && read_list(f, list).depth == 0u);
}

// Many threads that first touch the same fresh guest KEVENT at the same moment
// must all resolve the SAME host object. (Previously each could create its own,
// so a signal reached a different object than the one being waited on.)
void test_dispatcher_resolution_is_race_free() {
  Fixture f;
  constexpr int kHeaders = 200;
  constexpr int kThreads = 8;
  for (int round = 0; round < kHeaders; ++round) {
    const auto header = f.alloc(0x10);
    xbox::initialize_dispatch_header(*f.address_space, header,
                                     xbox::DispatchObjectType::EventNotification, 0u);
    std::vector<std::shared_ptr<kernel::KernelObject>> seen(kThreads);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        ++ready;
        while (!go.load()) std::this_thread::yield();
        std::string error;
        seen[t] = xbox::resolve_dispatcher_object(*f.process, *f.address_space, header, &error);
      });
    }
    while (ready.load() < kThreads) std::this_thread::yield();
    go = true;
    for (auto& th : threads) th.join();
    for (int t = 0; t < kThreads; ++t) {
      assert(seen[t] && seen[t].get() == seen[0].get() && "every thread resolves the same object");
    }
  }
}

// ---- Events, threads, misc -----------------------------------------------------
kernel::Handle insert(Fixture& f, std::shared_ptr<kernel::KernelObject> object) {
  kernel::Handle handle{};
  const auto code = f.process->handle_table().insert(std::move(object), 0x1F0003u, kernel::HandleFlags::None, handle);
  assert(code == kernel::KernelIoCode::Success);
  static_cast<void>(code);
  return handle;
}

void test_pulse_and_query_event() {
  Fixture f;
  const auto info = f.alloc(8);
  const auto manual = insert(f, std::make_shared<kernel::KernelEvent>(true, true));
  const auto autoreset = insert(f, std::make_shared<kernel::KernelEvent>(false, false));

  assert(f.call(kOrdNtQueryEvent, manual, info) == kStatusSuccess);
  assert(f.address_space->read32_be(info) == 0u && f.address_space->read32_be(info + 4u) == 1u);
  assert(f.call(kOrdNtQueryEvent, autoreset, info) == kStatusSuccess);
  assert(f.address_space->read32_be(info) == 1u && f.address_space->read32_be(info + 4u) == 0u);
  assert(f.call(kOrdNtQueryEvent, 0x7777u, info) == kStatusInvalidHandle);
  assert(f.call(kOrdNtQueryEvent, insert(f, std::make_shared<kernel::KernelEvent>(false, false)), 0u) ==
         0xC000000Du);

  // KePulseEvent on a guest KEVENT: reports the previous state and leaves it
  // unsignaled (a pulse releases waiters then resets).
  const auto kevent = f.alloc(0x10);
  xbox::initialize_dispatch_header(*f.address_space, kevent,
                                   xbox::DispatchObjectType::EventNotification, 1u);
  assert(f.call(kOrdKePulseEvent, kevent, 1, 0) == 1u && "previous state was signaled");
  assert(f.call(kOrdKePulseEvent, kevent, 1, 0) == 0u && "a pulse leaves it unsignaled");
}

void test_thread_suspension() {
  Fixture f;
  std::atomic<bool> release{false};
  kernel::ThreadCreationParams params{};
  params.name = "suspend-target";
  auto thread = f.process->thread_manager().create_thread(
      [&]() -> std::uint32_t {
        while (!release.load()) std::this_thread::sleep_for(1ms);
        return 0u;
      },
      params);
  assert(thread && thread->start());
  thread->set_guest_kthread_address(0x00445566u);
  const auto handle = insert(f, thread);

  const auto out = f.alloc(4);
  assert(f.call(kOrdNtSuspendThread, handle, out) == kStatusSuccess);
  assert(f.address_space->read32_be(out) == 0u && thread->suspend_count() == 1u);
  assert(f.call(kOrdNtSuspendThread, handle, out) == kStatusSuccess);
  assert(f.address_space->read32_be(out) == 1u && thread->suspend_count() == 2u);
  assert(f.call(kOrdKeSuspendThread, 0x00445566u) == 2u && "returns the previous count");
  assert(thread->suspend_count() == 3u);
  assert(f.call(kOrdKeSuspendThread, 0x00999999u) == 0u && "an unknown KTHREAD");

  assert(f.call(kOrdNtSuspendThread, 0x7777u, out) == kStatusInvalidHandle);
  assert(f.call(kOrdNtSuspendThread, insert(f, std::make_shared<kernel::KernelEvent>()), out) ==
         kStatusObjectTypeMismatch);

  // Boost-disable flag: returns the previous value.
  assert(f.call(kOrdKeSetDisableBoost, 0x00445566u, 1) == 0u);
  assert(f.call(kOrdKeSetDisableBoost, 0x00445566u, 0) == 1u);
  assert(f.call(kOrdKeSetDisableBoost, 0x00445566u, 0) == 0u);

  while (thread->suspend_count() > 0u) static_cast<void>(thread->resume());
  release = true;
  static_cast<void>(thread->join(5000));
  assert(f.call(kOrdNtSuspendThread, handle, out) == kStatusThreadIsTerminating);
}

void test_misc_process_free_exports() {
  Fixture f;
  assert(f.call(kOrdFscGet) == 0u);
  assert(f.call(kOrdFscSet, 0, 256) == kStatusSuccess);
  assert(f.call(kOrdFscGet) == 256u && "the configured count reads back");
  assert(f.call(kOrdYield) == kStatusSuccess);
  assert(f.call(kOrdKfRaiseIrql, 2) == 0u && "previous IRQL is PASSIVE");
  f.call(kOrdKeEnableFpuExceptions, 1);
}

}  // namespace

int main() {
  std::cout << "Testing threading exports...\n";
  struct Case {
    const char* name;
    void (*fn)();
  };
  const Case cases[] = {
      {"rwlock_initialize_and_uncontended", &test_rwlock_initialize_and_uncontended},
      {"rwlock_try_variants", &test_rwlock_try_variants},
      {"rwlock_writer_waits_for_reader", &test_rwlock_writer_waits_for_reader},
      {"rwlock_writer_hands_off_to_writer", &test_rwlock_writer_hands_off_to_writer},
      {"rwlock_readers_wait_for_writer_and_are_released_together",
       &test_rwlock_readers_wait_for_writer_and_are_released_together},
      {"rwlock_stress", &test_rwlock_stress},
      {"slist_push_pop_flush", &test_slist_push_pop_flush},
      {"slist_concurrent", &test_slist_concurrent},
      {"dispatcher_resolution_is_race_free", &test_dispatcher_resolution_is_race_free},
      {"pulse_and_query_event", &test_pulse_and_query_event},
      {"thread_suspension", &test_thread_suspension},
      {"misc_process_free_exports", &test_misc_process_free_exports},
  };
  for (const auto& c : cases) {
    std::cerr << "[test] " << c.name << std::endl;
    c.fn();
  }
  std::cout << "All threading export tests passed!\n";
  return 0;
}
