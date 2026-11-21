#include "xenon/xbox/xbox_time_convert.hpp"

#include <limits>

#include "xenon/kernel/time.hpp"

namespace xenon::xbox {

namespace {

constexpr std::int64_t kNanosecondsPer100Ns = 100;
constexpr std::uint64_t kMax100Ns =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)() / kNanosecondsPer100Ns);

std::chrono::nanoseconds from_100ns(std::uint64_t magnitude) {
  if (magnitude > kMax100Ns) return (std::chrono::nanoseconds::max)();
  return std::chrono::nanoseconds(static_cast<std::int64_t>(magnitude) * kNanosecondsPer100Ns);
}

}  // namespace

std::chrono::nanoseconds xbox_timeout_to_relative(std::int64_t raw_100ns) {
  if (raw_100ns == 0) {
    return std::chrono::nanoseconds(0);
  }
  if (raw_100ns < 0) {
    // Relative timeout: the magnitude is the delay from now. Negated as an
    // unsigned value, so INT64_MIN does not overflow.
    return from_100ns(0u - static_cast<std::uint64_t>(raw_100ns));
  }

  // Absolute timeout: measured on the same 1601-epoch clock
  // TimeServices::system_time() uses.
  const std::uint64_t now = kernel::TimeServices::system_time();
  const auto target = static_cast<std::uint64_t>(raw_100ns);
  if (target <= now) {
    return std::chrono::nanoseconds(0);
  }
  return from_100ns(target - now);
}

}  // namespace xenon::xbox
