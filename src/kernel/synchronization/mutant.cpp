#include "xenon/kernel/mutant.hpp"

#include "kernel/synchronization/wait_internal.hpp"
#include "xenon/kernel/wait_util.hpp"

namespace xenon::kernel {

KernelMutant::KernelMutant(bool initial_owner, std::uint32_t owner_thread_id)
    : KernelObject(ObjectType::Mutant) {
  if (initial_owner) {
    owner_thread_id_ = owner_thread_id;
    recursion_count_ = 1;
  }
}

bool KernelMutant::acquire(std::uint32_t owner_thread_id, std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);

  // Already owned by this thread: satisfied at once, deepening the recursion.
  // Otherwise wait for the mutant to become available.
  if (!wait_until_signaled(condition_, lock, timeout,
                           [&] { return can_satisfy_locked(owner_thread_id); })) {
    return false;
  }
  satisfy_locked(owner_thread_id);
  return true;
}

bool KernelMutant::release(std::uint32_t owner_thread_id) {
  std::scoped_lock lock(mutex_);

  if (owner_thread_id_ != owner_thread_id || recursion_count_ == 0) {
    return false;  // Not owned by this thread
  }

  --recursion_count_;
  if (recursion_count_ == 0) {
    owner_thread_id_ = 0;
    condition_.notify_one();
    detail::notify_multi_object_waiters();
  }

  return true;
}

bool KernelMutant::is_owned() const {
  std::scoped_lock lock(mutex_);
  return recursion_count_ > 0;
}

std::uint32_t KernelMutant::owner_thread_id() const {
  std::scoped_lock lock(mutex_);
  return owner_thread_id_;
}

std::int32_t KernelMutant::recursion_count() const {
  std::scoped_lock lock(mutex_);
  return recursion_count_;
}

void KernelMutant::abandon() {
  {
    std::scoped_lock lock(mutex_);
    abandoned_ = true;
    owner_thread_id_ = 0;
    recursion_count_ = 0;
  }
  condition_.notify_all();
  detail::notify_multi_object_waiters();
}

}  // namespace xenon::kernel
