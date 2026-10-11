#pragma once

// Shared failure categories for diagnostics and compatibility reports, so a
// report can say what kind of failure it found rather than only where.

#include <cstdint>
#include <string_view>

namespace xenon::logging {

enum class FailureCategory : std::uint8_t {
  // The guest used behaviour Xenon does not implement.
  Unsupported,
  // The guest passed something invalid (bad handle, pointer or argument).
  Invalid,
  // A bounded wait or operation ran out of time.
  Timeout,
  // A thread waits with no timeout and nothing recorded will wake it.
  DeadlockCandidate,
  // The guest faulted (memory access, trap, unhandled exception).
  GuestFault,
  // A native graphics/audio/input backend reported an error.
  NativeBackendError,
  // A native module or caller does not match the expected ABI.
  AbiMismatch,
  // Preparing or compiling a module failed.
  CompilationError,
  Unknown,
};

[[nodiscard]] constexpr std::string_view failure_category_name(FailureCategory category) noexcept {
  switch (category) {
    case FailureCategory::Unsupported: return "unsupported";
    case FailureCategory::Invalid: return "invalid";
    case FailureCategory::Timeout: return "timeout";
    case FailureCategory::DeadlockCandidate: return "deadlock candidate";
    case FailureCategory::GuestFault: return "guest fault";
    case FailureCategory::NativeBackendError: return "native backend error";
    case FailureCategory::AbiMismatch: return "ABI mismatch";
    case FailureCategory::CompilationError: return "compilation error";
    case FailureCategory::Unknown: return "unknown";
  }
  return "unknown";
}

}  // namespace xenon::logging
