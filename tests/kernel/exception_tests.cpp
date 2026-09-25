// Phase 5 (AC6 Runtime Readiness pass): ExceptionDispatcher::fault_to_exception().
//
// Regression coverage for a real bug the audit found: ExceptionInformation[0]
// (ExceptionRecord::parameters[0]) only ever encoded read (0) or write (1),
// collapsing an Execute-kind fault (the guest tried to run code from a
// non-executable page) into the "read" bucket. Real NT's
// EXCEPTION_RECORD.ExceptionInformation[0] uses 0 = read, 1 = write,
// 8 = execute/DEP violation - a stable, documented Win32/NT ABI value.
//
// Also documents (via test, not just comment) that every memory::FaultReason
// correctly maps to the SAME ExceptionCode::AccessViolation - that is real
// Xbox 360/NT behavior, not a bug to "fix" by inventing per-reason codes
// that have no verified real-hardware basis.

#include "xenon/kernel/exception.hpp"

#include <cassert>
#include <iostream>

using namespace xenon;

namespace {

kernel::ExceptionRecord fault_for(memory::AccessKind access, memory::FaultReason reason) {
  memory::MemoryFaultInfo info{};
  info.access = access;
  info.reason = reason;
  info.fault_address = 0x12345678u;
  return kernel::ExceptionDispatcher::fault_to_exception(info);
}

void test_read_fault_uses_information_zero() {
  const auto record = fault_for(memory::AccessKind::Read, memory::FaultReason::Unmapped);
  assert(record.code == kernel::ExceptionCode::AccessViolation);
  assert(record.parameters.size() == 2);
  assert(record.parameters[0] == 0u);
  assert(record.parameters[1] == 0x12345678u);
}

void test_write_fault_uses_information_one() {
  const auto record = fault_for(memory::AccessKind::Write, memory::FaultReason::Protection);
  assert(record.code == kernel::ExceptionCode::AccessViolation);
  assert(record.parameters[0] == 1u);
}

void test_execute_fault_uses_information_eight_not_read() {
  // The actual regression: previously fault.is_write() ? 1 : 0 silently
  // reported an execute-protection fault as a read (0).
  const auto record = fault_for(memory::AccessKind::Execute, memory::FaultReason::Protection);
  assert(record.code == kernel::ExceptionCode::AccessViolation);
  assert(record.parameters[0] == 8u &&
         "an Execute-kind fault must report NT's documented DEP-violation "
         "value (8), not be silently collapsed into the read bucket (0)");
}

void test_every_fault_reason_maps_to_access_violation() {
  // Real Xbox 360/NT behavior: FaultReason is Xenon's own internal
  // diagnostic taxonomy, not a guest-visible ABI surface - every reason
  // correctly produces the same STATUS_ACCESS_VIOLATION.
  const memory::FaultReason reasons[] = {
      memory::FaultReason::Unmapped, memory::FaultReason::Uncommitted,
      memory::FaultReason::Protection, memory::FaultReason::OutOfRange,
      memory::FaultReason::MmioWidth,
  };
  for (const auto reason : reasons) {
    const auto record = fault_for(memory::AccessKind::Read, reason);
    assert(record.code == kernel::ExceptionCode::AccessViolation);
  }
}

void test_dispatcher_handler_chain_most_recent_first() {
  kernel::ExceptionDispatcher dispatcher;
  int first_handler_calls = 0;
  int second_handler_calls = 0;

  dispatcher.register_handler([&](const kernel::ExceptionRecord&) {
    ++first_handler_calls;
    return false;  // keep searching
  });
  dispatcher.register_handler([&](const kernel::ExceptionRecord&) {
    ++second_handler_calls;
    return true;  // handled
  });

  const bool handled = dispatcher.dispatch_exception(kernel::ExceptionRecord{});
  assert(handled);
  // Most-recently-registered handler runs first and handles it, so the
  // first-registered handler must not be reached.
  assert(second_handler_calls == 1);
  assert(first_handler_calls == 0);
}

// Phase 5's actual deliverable: two guest threads' handler chains must be
// genuinely independent - a handler registered for thread A must never be
// consulted for thread B's exception, and vice versa.
void test_per_thread_handlers_are_independent() {
  kernel::ExceptionDispatcher dispatcher;
  constexpr std::uint32_t kThreadA = 1u;
  constexpr std::uint32_t kThreadB = 2u;

  int a_calls = 0;
  int b_calls = 0;
  dispatcher.register_thread_handler(kThreadA, [&](const kernel::ExceptionRecord&) {
    ++a_calls;
    return true;
  });
  dispatcher.register_thread_handler(kThreadB, [&](const kernel::ExceptionRecord&) {
    ++b_calls;
    return true;
  });

  assert(dispatcher.dispatch_exception(kernel::ExceptionRecord{}, kThreadA));
  assert(a_calls == 1 && b_calls == 0 &&
         "thread B's handler must not see thread A's exception");

  assert(dispatcher.dispatch_exception(kernel::ExceptionRecord{}, kThreadB));
  assert(a_calls == 1 && b_calls == 1 &&
         "thread A's handler must not see thread B's exception");
}

// A thread with no handler of its own still falls back to the process-wide
// chain - "independent" means scoped, not that per-thread registration is
// mandatory before a thread's faults are observable at all.
void test_thread_with_no_handler_falls_back_to_process_wide_chain() {
  kernel::ExceptionDispatcher dispatcher;
  constexpr std::uint32_t kThreadWithHandler = 1u;
  constexpr std::uint32_t kThreadWithoutHandler = 2u;

  int process_wide_calls = 0;
  dispatcher.register_thread_handler(kThreadWithHandler,
                                     [](const kernel::ExceptionRecord&) { return true; });
  dispatcher.register_handler([&](const kernel::ExceptionRecord&) {
    ++process_wide_calls;
    return true;
  });

  assert(dispatcher.dispatch_exception(kernel::ExceptionRecord{}, kThreadWithoutHandler));
  assert(process_wide_calls == 1);
}

// A thread-scoped handler is tried before the process-wide chain, and a
// non-claiming per-thread handler still lets the search fall through.
void test_thread_handler_tried_before_process_wide_chain() {
  kernel::ExceptionDispatcher dispatcher;
  constexpr std::uint32_t kThread = 1u;

  bool thread_handler_called = false;
  bool process_handler_called = false;
  dispatcher.register_thread_handler(kThread, [&](const kernel::ExceptionRecord&) {
    thread_handler_called = true;
    return false;  // decline; must fall through to the process-wide chain.
  });
  dispatcher.register_handler([&](const kernel::ExceptionRecord&) {
    process_handler_called = true;
    return true;
  });

  assert(dispatcher.dispatch_exception(kernel::ExceptionRecord{}, kThread));
  assert(thread_handler_called && process_handler_called);
}

// clear_thread_handlers() must fully remove a thread's chain - required so a
// guest thread's per-thread handlers cannot outlive it (see
// XenonSession::run_created_guest_thread()) or be silently inherited by a
// different, later use of the same slot.
void test_clear_thread_handlers_removes_the_whole_chain() {
  kernel::ExceptionDispatcher dispatcher;
  constexpr std::uint32_t kThread = 1u;

  int calls = 0;
  dispatcher.register_thread_handler(kThread, [&](const kernel::ExceptionRecord&) {
    ++calls;
    return true;
  });
  dispatcher.clear_thread_handlers(kThread);

  assert(!dispatcher.dispatch_exception(kernel::ExceptionRecord{}, kThread));
  assert(calls == 0);
}

// Thread id 0 is reserved ("no specific thread" - see KernelThread's
// monotonic id generation starting at 1) and must never accumulate a
// registered chain of its own.
void test_thread_id_zero_is_never_registered() {
  kernel::ExceptionDispatcher dispatcher;
  int calls = 0;
  dispatcher.register_thread_handler(0u, [&](const kernel::ExceptionRecord&) {
    ++calls;
    return true;
  });
  assert(!dispatcher.dispatch_exception(kernel::ExceptionRecord{}, 0u));
  assert(calls == 0);
}

}  // namespace

int main() {
  std::cout << "Testing ExceptionDispatcher::fault_to_exception()...\n";

  test_read_fault_uses_information_zero();
  test_write_fault_uses_information_one();
  test_execute_fault_uses_information_eight_not_read();
  test_every_fault_reason_maps_to_access_violation();
  test_dispatcher_handler_chain_most_recent_first();
  test_per_thread_handlers_are_independent();
  test_thread_with_no_handler_falls_back_to_process_wide_chain();
  test_thread_handler_tried_before_process_wide_chain();
  test_clear_thread_handlers_removes_the_whole_chain();
  test_thread_id_zero_is_never_registered();

  std::cout << "All exception dispatch tests passed!\n";
  return 0;
}
