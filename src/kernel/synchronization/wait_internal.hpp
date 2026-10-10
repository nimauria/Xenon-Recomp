#pragma once

// Private to the kernel's waitable objects and wait.cpp.
//
// Multi-object waits sleep on one kernel-wide wait generation rather than on
// each object's own condition variable. Every operation that can make a
// waitable object satisfiable calls one of these after changing that state.
// The generation's mutex is a leaf lock: nothing else is locked while it is
// held, so these may be called with an object's own mutex held.

namespace xenon::kernel::detail {

// For state guarded by the object's own mutex (events, semaphores, mutants,
// timers). Costs one atomic load when no multi-object wait is in progress: a
// waiter registers itself before it locks the objects it checks, so a signal
// that lands after that check observes the registration.
void notify_multi_object_waiters() noexcept;

// For state a waiter reads without the object's mutex (thread termination,
// published through KernelThread::is_terminated()). Always notifies.
void notify_multi_object_waiters_unconditionally() noexcept;

}  // namespace xenon::kernel::detail
