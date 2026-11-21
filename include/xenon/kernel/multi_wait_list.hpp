#pragma once

// Wait registration and the dispatcher lock. Private to the kernel's
// waitable objects and src/kernel/synchronization/wait.cpp; it is in a public
// header only because those objects hold a list.
//
// As in the NT dispatcher the Xbox 360 kernel derives from, every waitable
// object's state is guarded by one dispatcher lock, a wait registers one
// block on each object it waits on, and a change that makes an object
// satisfiable satisfies the registered waits at once, oldest first
// (KiWaitTest), consuming the object for them. A thread that starts waiting
// later can therefore never take a signal an earlier waiter was entitled to:
// a signalled object never has a waiter it could satisfy.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace xenon::kernel {
class KernelObject;
}

namespace xenon::kernel::detail {

// Guards the state of every event, semaphore, mutant and timer, and wait
// registration. Lock order: the dispatcher lock, then a list's; nothing else
// is locked after a list's.
std::mutex& dispatcher_mutex() noexcept;

// Blocks one host thread until another unparks it or a deadline passes; each
// host thread has one (wait.cpp). An unpark is kept until the next park, so a
// park after an unpark returns at once, and a park may return spuriously:
// the parked thread re-checks what it waits for. On Windows the deadline is
// a high-resolution waitable timer, so a timeout keeps sub-millisecond
// precision instead of rounding up to the next host millisecond.
class HostParker;

struct MultiWaitBlock {
  // What the wait is for, set before the block is registered, so a signaller
  // can test and satisfy it. `objects` is in the caller's index order (a
  // WaitAny takes the lowest satisfiable index); `distinct` holds each object
  // once. Empty for a block registered only to be woken (a delay).
  std::span<KernelObject* const> objects{};
  std::span<KernelObject* const> distinct{};
  bool wait_all{false};
  std::uint32_t thread_id{0};
  std::thread::id host_thread{};
  // The waiting host thread's parker.
  HostParker* parker{nullptr};

  // Set by a signaller, under the dispatcher lock, when it satisfied the
  // wait for the waiter: `index` and `abandoned` first, then `granted`
  // (release), so a waiter that reads `granted` (acquire) without the lock
  // reads them too. Once set it stays set.
  std::atomic<bool> granted{false};
  std::uint32_t index{0};
  bool abandoned{false};

  // Bumped (seq_cst) on every grant and every notify_always(); a waiter
  // that sees it unchanged since its last check parks. `parked` is the
  // waiter's half of the handshake: it is set (seq_cst) before the waiter
  // re-reads `wakeups` and parks, and a waker only unparks a waiter that
  // has it set, so a spinning waiter costs its waker no host call and no
  // wakeup is lost (either the waiter sees the new count or the waker sees
  // `parked`).
  std::atomic<std::uint64_t> wakeups{0};
  std::atomic<bool> parked{false};
};

class MultiWaitList {
 public:
  // Registration order is kept: it is the order waits are satisfied in.
  // A block that can still be satisfied (one with objects and not granted)
  // is added and removed only with the dispatcher lock held; a granted or
  // object-less block may be removed under this list's own lock alone (see
  // wait.cpp).
  void add(MultiWaitBlock* block);
  void remove(MultiWaitBlock* block) noexcept;

  // Call with the dispatcher lock held, after a change that can make the
  // object satisfiable: satisfies registered waits, oldest first, while they
  // can be satisfied, and wakes their waiters.
  void satisfy_waiters_locked() noexcept;
  // Wakes every registered waiter to re-check state it reads without the
  // dispatcher lock (thread termination, a queued APC, an alert).
  void notify_always() noexcept;
  [[nodiscard]] std::size_t size() const noexcept {
    return size_.load(std::memory_order_relaxed);
  }

  // Adaptive pre-block spinning (wait.cpp): whether a wait on this object
  // should spin before it blocks, from how recent spins on it ended.
  [[nodiscard]] bool spin_advised() noexcept;
  void note_spin(bool satisfied) noexcept;

 private:
  std::mutex mutex_;
  std::vector<MultiWaitBlock*> blocks_;
  std::atomic<std::uint32_t> size_{0};
  // Raised a lot by a spin the signal ended, lowered a little by one it did
  // not; a wait spins while it is above zero, and otherwise probes now and
  // then, so an object whose signals start arriving quickly earns it back.
  std::atomic<std::int32_t> spin_score_{16};
  std::atomic<std::uint32_t> spin_probe_{0};
};

}  // namespace xenon::kernel::detail
