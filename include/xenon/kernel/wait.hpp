#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "xenon/kernel/io_types.hpp"
#include "xenon/kernel/object.hpp"

namespace xenon::kernel {

enum class WaitResult : std::uint32_t {
  Success = 0,
  Timeout = 0x00000102,
  Abandoned = 0x00000080,
  // The waiting thread was terminated (KernelThread::terminate()) while it
  // waited, or before it began; the wait took nothing. STATUS_THREAD_IS_TERMINATING.
  Terminating = 0xC000004B,
  // An alertable wait or delay ended because a user APC is queued for the
  // waiting thread; the caller runs the APCs. STATUS_USER_APC.
  UserApc = 0x000000C0,
  // An alertable wait or delay ended because the waiting thread was alerted
  // (KeAlertThread); the alert is cleared. STATUS_ALERTED.
  Alerted = 0x00000101,
  Failed = 0xFFFFFFFF,
};

// Timeouts and delays keep the guest's 100 ns precision: a negative timeout,
// or one of 0xFFFFFFFF ms or more, waits forever; zero tests and returns; any
// other timeout ends the wait at a steady_clock deadline that is never early.
// (Existing callers that pass std::chrono::milliseconds convert implicitly.)
//
// A wait that cannot be satisfied at once registers and, before it blocks
// its host thread, spins for a few microseconds (wait_spin_limit()) watching
// its own grant: the signaller still satisfies and consumes the object for
// it under the dispatcher lock, exactly as for a blocked waiter, so
// auto-reset, mutant ownership, semaphore counts and APC/alert handling are
// unchanged; the spin only saves the block and the wakeup when the signal
// comes quickly. Whether a wait spins adapts per object to how recent spins
// on it ended (see MultiWaitList::spin_advised()).

// Wait for a single object to be signaled. waiting_thread_id identifies the
// calling guest thread and is only meaningful for ObjectType::Mutant (a
// mutant's ownership is thread-identity-scoped); pass the real calling
// thread's id whenever the caller has one (e.g. ExportCallContext::thread_id
// from a KeWaitForSingleObject/NtWaitForSingleObject export handler) rather
// than the default of 0, which acquire()s the mutant as "owned by no
// specific thread" and is only correct for callers that generically don't
// know their own thread identity.
[[nodiscard]] WaitResult wait_for_single_object(
    const std::shared_ptr<KernelObject>& object,
    std::chrono::nanoseconds timeout,
    std::uint32_t waiting_thread_id = 0,
    bool alertable = false);

// Wait for multiple objects. See wait_for_single_object() for
// waiting_thread_id. An alertable wait by a KernelThread returns UserApc
// when a user APC is queued for it and no object is satisfiable; an object
// that is satisfiable is taken first, as on NT.
[[nodiscard]] WaitResult wait_for_multiple_objects(
    std::span<const std::shared_ptr<KernelObject>> objects,
    bool wait_all,
    std::chrono::nanoseconds timeout,
    std::uint32_t* signaled_index = nullptr,
    std::uint32_t waiting_thread_id = 0,
    bool alertable = false);

// The wait every other wait is built on, over raw objects in index order
// (each must be waitable). Satisfied at once when it can be; otherwise the
// wait registers on every object and is satisfied by the signal that makes
// it satisfiable, ahead of any wait registered after it (see
// multi_wait_list.hpp).
[[nodiscard]] WaitResult wait_for_objects(std::span<KernelObject* const> objects, bool wait_all,
                                          std::chrono::nanoseconds timeout,
                                          std::uint32_t* signaled_index = nullptr,
                                          std::uint32_t waiting_thread_id = 0,
                                          bool alertable = false);

// NtSignalAndWaitForSingleObjectEx. `signal` runs with the dispatcher lock
// held (use the objects' *_locked operations) and the caller's wait on
// `object` is checked and registered in the same critical section, so no
// thread can act on the signal - take a mutant it released, then set the
// event this thread waits on - before this thread is waiting. Returns Failed
// without waiting when `signal` returns false.
[[nodiscard]] WaitResult signal_and_wait_for_object(const std::function<bool()>& signal,
                                                    const std::shared_ptr<KernelObject>& object,
                                                    std::chrono::nanoseconds timeout,
                                                    std::uint32_t waiting_thread_id = 0,
                                                    bool alertable = false);

// Sleeps the calling thread for `delay` (KeDelayExecutionThread), to 100 ns
// precision. A delay always gives up the processor: a zero or negative delay
// yields it once (yield_current_thread()), as a zero interval does on the
// Xbox 360, and a delay shorter than the host can block for precisely yields
// until it ends rather than returning at once. On a KernelThread the sleep
// ends early, returning Terminating, if the thread is terminated, or UserApc,
// if `alertable` and a user APC is queued for it; otherwise it returns Success.
[[nodiscard]] WaitResult delay_current_thread(std::chrono::nanoseconds delay,
                                              bool alertable = false);

// Gives the rest of the calling host thread's time slice to another ready
// thread (SwitchToThread on Windows, sched_yield elsewhere) - NtYieldExecution
// and a zero-interval KeDelayExecutionThread. Returns whether the host
// reported running another thread.
bool yield_current_thread() noexcept;

// The longest a registered wait spins before it blocks: 4 us unless
// XENON_WAIT_SPIN sets it (0 turns spinning off; N spins up to N us, at most
// 100). Never spins on a single-processor host.
[[nodiscard]] std::chrono::nanoseconds wait_spin_limit() noexcept;
// Overrides the limit (tests; a session option): zero turns spinning off.
void set_wait_spin_limit(std::chrono::nanoseconds limit) noexcept;

// The waits currently registered on a waitable object (0 for any other):
// for diagnostics and tests.
[[nodiscard]] std::size_t waiter_count(KernelObject& object);

// Helper to check if an object is waitable
[[nodiscard]] bool is_waitable_object(ObjectType type);

// Helper to wait on specific object types. See wait_for_single_object() for
// waiting_thread_id.
[[nodiscard]] bool wait_on_object(KernelObject& object,
                                  std::chrono::nanoseconds timeout,
                                  std::uint32_t waiting_thread_id = 0);

}  // namespace xenon::kernel
