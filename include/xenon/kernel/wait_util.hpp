#pragma once

#include <chrono>
#include <condition_variable>

namespace xenon::kernel {

// Waits on `condition` until `predicate` holds or `timeout` elapses; returns
// whether the predicate held.
//
// A NEGATIVE timeout is the "wait forever" sentinel (xbox_infinite_timeout() and
// a wait call's NULL timeout pointer both produce it, and 0xFFFFFFFF ms is the
// same convention elsewhere in the kernel). std::condition_variable::wait_for
// treats a negative duration as one that has already expired, so passing the
// sentinel straight through returns "timed out" immediately - an infinite wait
// that never waits. Every kernel object's wait goes through this helper instead.
template <class Lock, class Predicate>
[[nodiscard]] bool wait_until_signaled(std::condition_variable& condition, Lock& lock,
                                       std::chrono::milliseconds timeout, Predicate predicate) {
  if (timeout.count() < 0 || timeout.count() >= 0xFFFFFFFFll) {
    condition.wait(lock, predicate);
    return true;
  }
  return condition.wait_for(lock, timeout, predicate);
}

}  // namespace xenon::kernel
