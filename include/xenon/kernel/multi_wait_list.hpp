#pragma once

// Multi-object wait registration. Private to the kernel's waitable objects
// and src/kernel/synchronization/wait.cpp; it is in a public header only
// because those objects hold a list.
//
// A multi-object wait registers one block on each object it waits on and
// sleeps on that block. A change that can make an object satisfiable wakes
// only the blocks registered on that object, as the NT dispatcher's
// per-object wait lists do, rather than every multi-object wait in progress.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace xenon::kernel::detail {

struct MultiWaitBlock {
  std::mutex mutex;
  std::condition_variable condition;
  std::uint64_t wakeups{0};
};

// Lock order: an object's own mutex, then its list's, then a block's. The
// list's and the block's are taken with nothing else locked after them.
class MultiWaitList {
 public:
  void add(MultiWaitBlock* block);
  void remove(MultiWaitBlock* block) noexcept;

  // Call after a change, made under the object's own mutex, that can make the
  // object satisfiable. Costs one atomic load when no multi-object wait is
  // registered: a waiter registers before it locks the object to check it, so
  // a change after that check observes the registration.
  void notify() noexcept {
    if (size_.load(std::memory_order_seq_cst) != 0) notify_always();
  }
  // For state a waiter reads without the object's mutex (thread termination).
  void notify_always() noexcept;

 private:
  std::mutex mutex_;
  std::vector<MultiWaitBlock*> blocks_;
  std::atomic<std::uint32_t> size_{0};
};

}  // namespace xenon::kernel::detail
