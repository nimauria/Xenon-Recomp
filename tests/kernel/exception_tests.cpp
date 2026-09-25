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

}  // namespace

int main() {
  std::cout << "Testing ExceptionDispatcher::fault_to_exception()...\n";

  test_read_fault_uses_information_zero();
  test_write_fault_uses_information_one();
  test_execute_fault_uses_information_eight_not_read();
  test_every_fault_reason_maps_to_access_violation();
  test_dispatcher_handler_chain_most_recent_first();

  std::cout << "All exception dispatch tests passed!\n";
  return 0;
}
