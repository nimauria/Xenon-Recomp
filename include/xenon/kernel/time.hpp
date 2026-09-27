#pragma once

#include <chrono>
#include <cstdint>

namespace xenon::kernel {

// Xbox 360 time services
class TimeServices {
 public:
  // Get system time in 100-nanosecond intervals since Jan 1, 1601 (FILETIME format)
  [[nodiscard]] static std::uint64_t system_time();

  // Get high-resolution performance counter (in ticks)
  [[nodiscard]] static std::uint64_t performance_counter();

  // Get performance counter frequency (ticks per second)
  [[nodiscard]] static std::uint64_t performance_frequency();

  // Convert milliseconds to Xbox time units
  [[nodiscard]] static std::uint64_t milliseconds_to_xbox_time(std::uint64_t milliseconds);

  // Convert Xbox time units to milliseconds
  [[nodiscard]] static std::uint64_t xbox_time_to_milliseconds(std::uint64_t xbox_time);

  // Sleep for the specified number of milliseconds
  static void sleep(std::uint32_t milliseconds);

  // High-precision delay
  static void delay_execution(std::chrono::microseconds duration);

 private:
  // Epoch offset between Unix epoch (1970) and Windows epoch (1601)
  static constexpr std::uint64_t kEpochDifference = 11644473600ULL;
  // 100-nanosecond intervals per second
  static constexpr std::uint64_t kIntervalsPerSecond = 10000000ULL;

 public:
  // The real Xbox 360 CPU's PPC time-base register runs at a fixed 50 MHz,
  // independent of CPU clock scaling. Verified against xenia-project/xenia
  // (this project's primary Xbox 360 semantic reference) rather than
  // guessed: src/xenia/emulator.cc calls
  // Clock::set_guest_tick_frequency(50000000) with the comment "360 uses a
  // 50MHz clock". performance_frequency() (what KeQueryPerformanceFrequency
  // reports to guest code) and performance_counter() (what
  // XenonSession::read_time_base() derives the real mftb/mftbu value from)
  // must both use this same rate - real guest code computes elapsed time
  // as (tb_delta) / KeQueryPerformanceFrequency(), so the two are one
  // coherent clock domain, not two independently-chosen numbers.
  static constexpr std::uint64_t kGuestTimeBaseFrequencyHz = 50000000ULL;
};

}  // namespace xenon::kernel
