#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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

// Exception dispatcher - connects Memory V2 faults to guest exception behavior
class ExceptionDispatcher {
 public:
  ExceptionDispatcher() = default;

  // Register an exception handler
  void register_handler(ExceptionHandler handler);

  // Remove all handlers
  void clear_handlers();

  // Dispatch a memory fault as an exception
  [[nodiscard]] bool dispatch_memory_fault(const memory::MemoryFaultInfo& fault);

  // Dispatch a generic exception
  [[nodiscard]] bool dispatch_exception(const ExceptionRecord& record);

  // Convert memory fault to exception record
  [[nodiscard]] static ExceptionRecord fault_to_exception(
      const memory::MemoryFaultInfo& fault);

 private:
  std::vector<ExceptionHandler> handlers_;
};

}  // namespace xenon::kernel
