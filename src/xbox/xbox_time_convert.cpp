#include "xenon/xbox/xbox_time_convert.hpp"

#include "xenon/kernel/time.hpp"

namespace xenon::xbox {

std::chrono::milliseconds xbox_timeout_to_relative_ms(std::int64_t raw_100ns) {
  constexpr std::int64_t kHundredNsPerMs = 10'000;

  if (raw_100ns == 0) {
    return std::chrono::milliseconds(0);
  }
  if (raw_100ns < 0) {
    // Relative timeout: the magnitude is the delay from now.
    const std::int64_t magnitude = -raw_100ns;
    return std::chrono::milliseconds(magnitude / kHundredNsPerMs);
  }

  // Absolute timeout: measured on the same 1601-epoch clock
  // TimeServices::system_time() uses.
  const std::uint64_t now = kernel::TimeServices::system_time();
  const auto target = static_cast<std::uint64_t>(raw_100ns);
  if (target <= now) {
    return std::chrono::milliseconds(0);
  }
  const std::uint64_t delta = target - now;
  return std::chrono::milliseconds(static_cast<std::int64_t>(delta / kHundredNsPerMs));
}

}  // namespace xenon::xbox
