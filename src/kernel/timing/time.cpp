#include "xenon/kernel/time.hpp"

#include <thread>

namespace xenon::kernel {

std::uint64_t TimeServices::system_time() {
  // Get current time as Unix timestamp
  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
  auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() % 1000000000;

  // Convert to Windows FILETIME format (100-nanosecond intervals since 1601)
  std::uint64_t intervals = (seconds + kEpochDifference) * kIntervalsPerSecond;
  intervals += nanos / 100;

  return intervals;
}

std::uint64_t TimeServices::performance_counter() {
  // steady_clock, not high_resolution_clock: the real PPC time-base
  // register can never go backward, and high_resolution_clock carries no
  // such guarantee (it may alias system_clock on some implementations).
  // Scaled from host nanoseconds down to real 50 MHz Xbox 360 ticks (one
  // tick per 20ns) - see kGuestTimeBaseFrequencyHz's doc comment.
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  constexpr std::uint64_t kNanosecondsPerGuestTick =
      1000000000ULL / kGuestTimeBaseFrequencyHz;
  return static_cast<std::uint64_t>(ns) / kNanosecondsPerGuestTick;
}

std::uint64_t TimeServices::performance_frequency() {
  return kGuestTimeBaseFrequencyHz;
}

std::uint64_t TimeServices::milliseconds_to_xbox_time(std::uint64_t milliseconds) {
  return milliseconds * 10000ULL;  // Convert ms to 100-ns intervals
}

std::uint64_t TimeServices::xbox_time_to_milliseconds(std::uint64_t xbox_time) {
  return xbox_time / 10000ULL;  // Convert 100-ns intervals to ms
}

void TimeServices::sleep(std::uint32_t milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

void TimeServices::delay_execution(std::chrono::microseconds duration) {
  std::this_thread::sleep_for(duration);
}

}  // namespace xenon::kernel
