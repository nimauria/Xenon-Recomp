// Phase 3 (AC6 Runtime Readiness pass): synchronization object correctness.
//
// Regression coverage for two real bugs the audit found before any guest-
// callable export existed to exercise them:
//   1. KernelMutant's constructor hardcoded owner_thread_id_ = 1, which
//      collides with the first real guest thread id ThreadManager/
//      KernelThread ever allocates (see thread.cpp's g_next_thread_id).
//   2. wait_on_object()/wait_for_single_object() for ObjectType::Mutant
//      hardcoded the waiting thread's id to 0, losing thread identity for
//      any caller that goes through the generic wait path (as every
//      KeWaitForSingleObject/NtWaitForSingleObject export handler does).

#include "xenon/kernel/event.hpp"
#include "xenon/kernel/mutant.hpp"
#include "xenon/kernel/semaphore.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/kernel/wait.hpp"

#include <cassert>
#include <iostream>
#include <memory>

using namespace xenon::kernel;

namespace {

void test_mutant_owner_id_does_not_collide_with_first_thread_id() {
  // Reproduces the exact collision scenario: the first KernelThread this
  // process ever creates gets guest thread id 1 (thread.cpp's file-scope
  // g_next_thread_id atomic starts at 1 and is shared process-wide, matching
  // a real ThreadManager's first-thread allocation).
  ThreadManager manager;
  ThreadCreationParams params{};
  auto first_thread = manager.create_thread([]() -> std::uint32_t { return 0; }, params);
  assert(first_thread != nullptr);
  assert(first_thread->thread_id() == 1);

  // A mutant created with a distinct real owner (e.g. thread id 42) must
  // report that real owner, never the old hardcoded placeholder of 1 -
  // otherwise it would appear pre-owned by first_thread above.
  KernelMutant mutant(/*initial_owner=*/true, /*owner_thread_id=*/42);
  assert(mutant.is_owned());
  assert(mutant.owner_thread_id() == 42);
  assert(mutant.owner_thread_id() != first_thread->thread_id());
}

void test_mutant_default_construction_has_no_owner() {
  KernelMutant mutant;  // initial_owner defaults to false
  assert(!mutant.is_owned());
  assert(mutant.owner_thread_id() == 0);
}

void test_wait_on_object_propagates_calling_thread_identity_to_mutant() {
  auto mutant = std::make_shared<KernelMutant>(/*initial_owner=*/false);
  assert(!mutant->is_owned());

  // Going through the generic wait path (exactly what a
  // KeWaitForSingleObject/NtWaitForSingleObject export handler does) with a
  // real waiting_thread_id must make the mutant report that thread as owner,
  // not the old hardcoded 0.
  const WaitResult result =
      wait_for_single_object(mutant, std::chrono::milliseconds(0), /*waiting_thread_id=*/99);
  assert(result == WaitResult::Success);
  assert(mutant->is_owned());
  assert(mutant->owner_thread_id() == 99);

  // Reacquiring (recursive) from the SAME thread id succeeds and increments
  // recursion count rather than blocking.
  const WaitResult reacquire =
      wait_for_single_object(mutant, std::chrono::milliseconds(0), /*waiting_thread_id=*/99);
  assert(reacquire == WaitResult::Success);
  assert(mutant->recursion_count() == 2);

  assert(mutant->release(99));
  assert(mutant->release(99));
  assert(!mutant->is_owned());
}

void test_wait_on_object_default_thread_id_is_explicit_zero() {
  // A caller that does not know its own thread identity (waiting_thread_id
  // left at its default) still gets deterministic, documented behavior:
  // the mutant is acquired as owned by thread id 0, not by whatever thread
  // happened to be created first in this process.
  auto mutant = std::make_shared<KernelMutant>(/*initial_owner=*/false);
  const WaitResult result = wait_for_single_object(mutant, std::chrono::milliseconds(0));
  assert(result == WaitResult::Success);
  assert(mutant->owner_thread_id() == 0);
  assert(mutant->release(0));
}

void test_event_and_semaphore_still_wait_correctly_through_generic_path() {
  // Regression safety net for the wait_on_object()/wait_for_single_object()
  // signature change (added waiting_thread_id): non-mutant object types must
  // be completely unaffected by the new parameter.
  auto event = std::make_shared<KernelEvent>(/*manual_reset=*/true, /*initial_state=*/false);
  assert(wait_for_single_object(event, std::chrono::milliseconds(0)) == WaitResult::Timeout);
  event->set();
  assert(wait_for_single_object(event, std::chrono::milliseconds(0)) == WaitResult::Success);

  auto semaphore = std::make_shared<KernelSemaphore>(/*initial_count=*/0, /*maximum_count=*/1);
  assert(wait_for_single_object(semaphore, std::chrono::milliseconds(0)) == WaitResult::Timeout);
  std::int32_t previous = 0;
  assert(semaphore->release(1, &previous));
  assert(previous == 0);
  assert(wait_for_single_object(semaphore, std::chrono::milliseconds(0)) == WaitResult::Success);
}

}  // namespace

int main() {
  std::cout << "Testing kernel synchronization objects...\n";

  test_mutant_owner_id_does_not_collide_with_first_thread_id();
  test_mutant_default_construction_has_no_owner();
  test_wait_on_object_propagates_calling_thread_identity_to_mutant();
  test_wait_on_object_default_thread_id_is_explicit_zero();
  test_event_and_semaphore_still_wait_correctly_through_generic_path();

  std::cout << "All kernel synchronization tests passed!\n";
  return 0;
}
