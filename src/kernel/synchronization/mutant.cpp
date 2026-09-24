#include "xenon/kernel/mutant.hpp"

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

  // If already owned by this thread, increment recursion count
  if (owner_thread_id_ == owner_thread_id && recursion_count_ > 0) {
    ++recursion_count_;
    return true;
  }

  // Wait for the mutant to become available
  if (!condition_.wait_for(lock, timeout, [&] { return recursion_count_ == 0; })) {
    return false;
  }

  owner_thread_id_ = owner_thread_id;
  recursion_count_ = 1;
  abandoned_ = false;
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
}

}  // namespace xenon::kernel
