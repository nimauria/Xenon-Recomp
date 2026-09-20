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
  ExceptionRecord record;
  
  switch (fault.reason) {
    case memory::FaultReason::Unmapped:
    case memory::FaultReason::Uncommitted:
    case memory::FaultReason::Protection:
      record.code = ExceptionCode::AccessViolation;
      record.address = static_cast<std::uint32_t>(fault.fault_address);
      record.parameters.push_back(fault.is_write() ? 1 : 0);  // Write/Read
      record.parameters.push_back(static_cast<std::uint32_t>(fault.fault_address));
      break;

    default:
      record.code = ExceptionCode::AccessViolation;
      record.address = static_cast<std::uint32_t>(fault.fault_address);
      record.parameters.push_back(fault.is_write() ? 1 : 0);
      record.parameters.push_back(static_cast<std::uint32_t>(fault.fault_address));
      break;
  }

  return record;
}

}  // namespace xenon::kernel
