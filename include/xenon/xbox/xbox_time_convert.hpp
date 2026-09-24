#pragma once

#include <chrono>
#include <cstdint>

namespace xenon::xbox {

// Converts an NT/Xbox 360 LARGE_INTEGER timeout value (100-nanosecond units;
// negative = relative delay from now, positive = absolute time measured on
// the same 1601-epoch clock as kernel::TimeServices::system_time(), zero =
// return immediately) into a relative std::chrono::milliseconds duration
// suitable for xenon::kernel's wait_for_single_object()/
// wait_for_multiple_objects()/KernelTimer, which only understand relative
// durations. Shared by the timer/wait/delay-execution export handlers so the
// conversion (and its rounding behavior) is defined exactly once.
[[nodiscard]] std::chrono::milliseconds xbox_timeout_to_relative_ms(std::int64_t raw_100ns);

// The sentinel xenon::kernel's wait functions already treat as "wait
// forever" (see wait.cpp), reused here so callers that need to represent a
// NULL/absent Xbox timeout pointer (real NT/Xbox semantics: NULL timeout on
// a wait call means INFINITE) do not need to know that encoding themselves.
[[nodiscard]] constexpr std::chrono::milliseconds xbox_infinite_timeout() {
  return std::chrono::milliseconds(-1);
}

}  // namespace xenon::xbox
