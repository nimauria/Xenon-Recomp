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
#include <condition_variable>
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
// registration. Lock order: the dispatcher lock, then a list's, then a
// block's; nothing else is locked after a block's.
std::mutex& dispatcher_mutex() noexcept;

struct MultiWaitBlock {
  std::mutex mutex;
  std::condition_variable condition;
  std::uint64_t wakeups{0};

  // What the wait is for, set before the block is registered, so a signaller
  // can test and satisfy it. `objects` is in the caller's index order (a
  // WaitAny takes the lowest satisfiable index); `distinct` holds each object
  // once. Empty for a block registered only to be woken (a delay).
  std::span<KernelObject* const> objects{};
  std::span<KernelObject* const> distinct{};
  bool wait_all{false};
  std::uint32_t thread_id{0};
  std::thread::id host_thread{};

  // Set by a signaller, under the dispatcher lock and this block's mutex,
  // when it satisfied the wait for the waiter.
  bool granted{false};
  std::uint32_t index{0};
  bool abandoned{false};
};

class MultiWaitList {
 public:
  // Registration order is kept: it is the order waits are satisfied in.
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

 private:
  std::mutex mutex_;
  std::vector<MultiWaitBlock*> blocks_;
  std::atomic<std::uint32_t> size_{0};
};

}  // namespace xenon::kernel::detail
