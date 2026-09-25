#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "xenon/memory/fault.hpp"

namespace xenon::kernel {

// Xbox 360 exception codes
enum class ExceptionCode : std::uint32_t {
  AccessViolation = 0xC0000005,
  IllegalInstruction = 0xC000001D,
  IntegerDivideByZero = 0xC0000094,
  IntegerOverflow = 0xC0000095,
  PrivilegedInstruction = 0xC0000096,
  // A `bl`/`bctrl`-style call target resolved to neither a discovered/
  // compiled guest function nor a recognized XEX import thunk (or a
  // recognized import whose specific export this build does not implement).
  // Matches real NT's STATUS_PROCEDURE_NOT_FOUND - see XenonSession::call().
  ProcedureNotFound = 0xC000007A,
  StackOverflow = 0xC00000FD,
  FloatingPointDenormal = 0xC000008D,
  FloatingPointDivideByZero = 0xC000008E,
  FloatingPointInexact = 0xC000008F,
  FloatingPointInvalid = 0xC0000090,
  FloatingPointOverflow = 0xC0000091,
  FloatingPointUnderflow = 0xC0000092,
  Breakpoint = 0x80000003,
  SingleStep = 0x80000004,
};

struct ExceptionRecord {
  ExceptionCode code{ExceptionCode::AccessViolation};
  std::uint32_t flags{0};
  std::uint32_t address{0};
  std::vector<std::uint32_t> parameters{};
};

// Exception handler callback
// Returns true if the exception was handled, false to continue searching
using ExceptionHandler = std::function<bool(const ExceptionRecord&)>;

// Exception dispatcher - connects Memory V2 faults to guest exception behavior.
//
// Per-thread independent exception-handler chains (AC6 Runtime Readiness
// pass, Phase 5): real Xbox 360/PPC structured exception handling searches a
// chain rooted at the FAULTING thread's own current try/except frame before
// ever considering anything process-wide. Reproducing the exact guest-memory
// EXCEPTION_REGISTRATION_RECORD frame-chain layout (walked via compiler-
// generated .pdata/unwind info) is Part 13 work and is not attempted here -
// see docs/kernel/THREADING_V2.md. What IS implemented here, at the layer
// Xenon's exception dispatch actually operates at (a host-side C++ handler
// chain, not compiled-in guest frames), is the same search *order*: a
// handler registered for one specific guest thread is tried first and only
// for that thread's own exceptions, before falling back to the process-wide
// chain every thread shares - so one thread's handler can no longer
// silently see or swallow another thread's fault, which the previous
// single, global handlers_ vector could not guarantee.
class ExceptionDispatcher {
 public:
  ExceptionDispatcher() = default;

  // Registers a process-wide handler, tried for every guest thread's
  // exceptions once that thread's own handlers (if any) decline to handle
  // it. Thread-safe: may be called from any guest-executing host thread.
  void register_handler(ExceptionHandler handler);

  // Removes every process-wide handler.
  void clear_handlers();

  // Registers a handler scoped to one specific guest thread id (see
  // kernel::KernelThread::thread_id() - ids start at 1, so 0 is never a
  // real thread and is reserved to mean "no specific thread" at dispatch
  // call sites that have none). Tried before the process-wide chain, and
  // only for that thread's own exceptions.
  void register_thread_handler(std::uint32_t thread_id, ExceptionHandler handler);

  // Removes every handler registered for this thread id. Callers release a
  // guest thread's per-thread chain once the thread has fully exited (see
  // XenonSession::run_created_guest_thread()) so a stale handler can never
  // outlive - or be silently inherited by - a different, later thread.
  void clear_thread_handlers(std::uint32_t thread_id);

  // Dispatch a memory fault as an exception, scoped to faulting_thread_id
  // (0 = no specific thread; searches the process-wide chain only).
  [[nodiscard]] bool dispatch_memory_fault(const memory::MemoryFaultInfo& fault,
                                           std::uint32_t faulting_thread_id = 0u);

  // Dispatch a generic exception, scoped to faulting_thread_id (0 = no
  // specific thread; searches the process-wide chain only).
  [[nodiscard]] bool dispatch_exception(const ExceptionRecord& record,
                                        std::uint32_t faulting_thread_id = 0u);

  // Convert memory fault to exception record
  [[nodiscard]] static ExceptionRecord fault_to_exception(
      const memory::MemoryFaultInfo& fault);

 private:
  // Handler registration/removal can race real concurrent guest thread
  // creation/exit; dispatch itself (memory faults, traps) already happens
  // off the safepoint-checked guest dispatch path, so this stays a plain
  // mutex rather than anything lock-free.
  std::mutex mutex_{};
  std::vector<ExceptionHandler> handlers_{};
  std::unordered_map<std::uint32_t, std::vector<ExceptionHandler>> thread_handlers_{};
};

}  // namespace xenon::kernel
