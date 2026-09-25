#include "xenon/kernel/exception.hpp"

namespace xenon::kernel {

void ExceptionDispatcher::register_handler(ExceptionHandler handler) {
  if (!handler) return;
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_.push_back(std::move(handler));
}

void ExceptionDispatcher::clear_handlers() {
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_.clear();
}

void ExceptionDispatcher::register_thread_handler(std::uint32_t thread_id,
                                                   ExceptionHandler handler) {
  if (!handler || thread_id == 0u) return;
  std::lock_guard<std::mutex> lock(mutex_);
  thread_handlers_[thread_id].push_back(std::move(handler));
}

void ExceptionDispatcher::clear_thread_handlers(std::uint32_t thread_id) {
  if (thread_id == 0u) return;
  std::lock_guard<std::mutex> lock(mutex_);
  thread_handlers_.erase(thread_id);
}

bool ExceptionDispatcher::dispatch_memory_fault(const memory::MemoryFaultInfo& fault,
                                                std::uint32_t faulting_thread_id) {
  auto record = fault_to_exception(fault);
  return dispatch_exception(record, faulting_thread_id);
}

bool ExceptionDispatcher::dispatch_exception(const ExceptionRecord& record,
                                             std::uint32_t faulting_thread_id) {
  // Snapshot the relevant chains under the lock, then invoke handlers
  // outside it: a handler may itself register/clear handlers (e.g. logging
  // and then reinstalling itself), which would otherwise re-enter mutex_.
  std::vector<ExceptionHandler> thread_chain;
  std::vector<ExceptionHandler> process_chain;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (faulting_thread_id != 0u) {
      const auto it = thread_handlers_.find(faulting_thread_id);
      if (it != thread_handlers_.end()) thread_chain = it->second;
    }
    process_chain = handlers_;
  }

  // Real per-thread SEH search order: the faulting thread's own handlers
  // are tried first (most recently registered first), and only if none of
  // them claim the exception does the search fall back to the process-wide
  // chain every thread shares. This is what makes the chains "independent"
  // - a handler registered for thread A is never even consulted for thread
  // B's fault, and vice versa.
  for (auto it = thread_chain.rbegin(); it != thread_chain.rend(); ++it) {
    if ((*it)(record)) return true;
  }
  for (auto it = process_chain.rbegin(); it != process_chain.rend(); ++it) {
    if ((*it)(record)) return true;
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
