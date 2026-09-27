#include "xenon/kernel/semaphore.hpp"

#include <algorithm>

namespace xenon::kernel {

KernelSemaphore::KernelSemaphore(std::int32_t initial_count, std::int32_t maximum_count)
    : KernelObject(ObjectType::Semaphore),
      count_(initial_count),
      maximum_count_(maximum_count) {}

bool KernelSemaphore::release(std::int32_t release_count, std::int32_t* previous_count) {
  if (release_count <= 0) {
    return false;
  }

  {
    std::scoped_lock lock(mutex_);
    if (previous_count) {
      *previous_count = count_;
    }

    if (count_ + release_count > maximum_count_) {
      return false;  // Would exceed maximum
    }

    count_ += release_count;
  }

  // Notify waiting threads
  if (release_count == 1) {
    condition_.notify_one();
  } else {
    condition_.notify_all();
  }

  return true;
}

bool KernelSemaphore::wait_for(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  if (!condition_.wait_for(lock, timeout, [&] { return count_ > 0; })) {
    return false;
  }
  --count_;
  return true;
}

std::int32_t KernelSemaphore::count() const {
  std::scoped_lock lock(mutex_);
  return count_;
}

}  // namespace xenon::kernel
