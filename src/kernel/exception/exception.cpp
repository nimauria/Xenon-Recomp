#include "xenon/kernel/exception.hpp"

namespace xenon::kernel {

void ExceptionDispatcher::register_handler(ExceptionHandler handler) {
  if (handler) {
    handlers_.push_back(std::move(handler));
  }
}

void ExceptionDispatcher::clear_handlers() {
  handlers_.clear();
}

bool ExceptionDispatcher::dispatch_memory_fault(const memory::MemoryFaultInfo& fault) {
  auto record = fault_to_exception(fault);
  return dispatch_exception(record);
}

bool ExceptionDispatcher::dispatch_exception(const ExceptionRecord& record) {
  // Try each handler in reverse order (most recently registered first)
  for (auto it = handlers_.rbegin(); it != handlers_.rend(); ++it) {
    if ((*it)(record)) {
      return true;  // Exception was handled
    }
  }
  return false;  // No handler handled the exception
}

ExceptionRecord ExceptionDispatcher::fault_to_exception(
    const memory::MemoryFaultInfo& fault) {
  // Every memory::FaultReason (Unmapped, Uncommitted, Protection,
  // OutOfRange, MmioWidth) maps to the same STATUS_ACCESS_VIOLATION here -
  // that reflects real Xbox 360/NT semantics correctly: none of those
  // reasons has a distinct, separately-documented NTSTATUS code a guest
  // __except filter could observe. FaultReason is Xenon's own internal
  // Memory V2 diagnostic taxonomy (useful for host-side debugging via the
  // diagnostic string built in XenonSession::dispatch_guest_thread()'s
  // MemoryFault catch clause), not a guest-visible ABI surface.
  //
  // What WAS a real bug: ExceptionInformation[0] (record.parameters[0])
  // only ever encoded read (0) or write (1), collapsing an Execute-kind
  // fault (the guest tried to run code from a non-executable page) into the
  // "read" bucket. Real NT's EXCEPTION_RECORD.ExceptionInformation[0] uses
  // 0 = read, 1 = write, 8 = execute/DEP violation - a stable, documented
  // Win32/NT ABI value, not an Xbox-specific unknown - so a guest __except
  // filter that branches on this value now sees the correct one.
  ExceptionRecord record;
  record.code = ExceptionCode::AccessViolation;
  record.address = static_cast<std::uint32_t>(fault.fault_address);

  std::uint32_t access_flag = 0;  // read
  if (fault.is_write()) {
    access_flag = 1;
  } else if (fault.is_execute()) {
    access_flag = 8;
  }
  record.parameters.push_back(access_flag);
  record.parameters.push_back(static_cast<std::uint32_t>(fault.fault_address));

  return record;
}

}  // namespace xenon::kernel
