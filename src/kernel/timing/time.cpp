#include "xenon/kernel/time.hpp"

#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

namespace xenon::kernel {
namespace {
#if defined(_WIN32)
// ntdll's timer-resolution calls, in 100 ns units. Unlike timeBeginPeriod
// they reach the hardware's finest step (0.5 ms on current Windows) and report
// the resolution actually in force.
using NtQueryTimerResolutionFn = LONG(NTAPI*)(PULONG, PULONG, PULONG);
using NtSetTimerResolutionFn = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);

template <typename Fn>
Fn ntdll_function(const char* name) {
  const auto module = GetModuleHandleW(L"ntdll.dll");
  return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
}
#endif
}  // namespace

HostTimerResolution::HostTimerResolution() {
#if defined(_WIN32)
  const auto query = ntdll_function<NtQueryTimerResolutionFn>("NtQueryTimerResolution");
  const auto set = ntdll_function<NtSetTimerResolutionFn>("NtSetTimerResolution");
  if (!query || !set) return;
  ULONG coarsest = 0, finest = 0, current = 0;
  if (query(&coarsest, &finest, &current) < 0) return;
  // Windows 11 drops a process's resolution request while its window is
  // hidden unless the process opts out of that throttling.
  PROCESS_POWER_THROTTLING_STATE throttling{};
  throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  throttling.StateMask = 0;
  static_cast<void>(SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                                          &throttling, sizeof(throttling)));
  ULONG granted = 0;
  if (set(finest, TRUE, &granted) < 0) return;
  requested_ = finest;
  active_ = true;
#else
  timespec resolution{};
  active_ = clock_getres(CLOCK_MONOTONIC, &resolution) == 0 &&
            resolution.tv_sec == 0 && resolution.tv_nsec <= 1'000'000;
#endif
}

HostTimerResolution::~HostTimerResolution() {
#if defined(_WIN32)
  if (!active_) return;
  if (const auto set = ntdll_function<NtSetTimerResolutionFn>("NtSetTimerResolution")) {
    ULONG current = 0;
    static_cast<void>(set(requested_, FALSE, &current));
  }
#endif
}

std::chrono::nanoseconds HostTimerResolution::current() {
#if defined(_WIN32)
  const auto query = ntdll_function<NtQueryTimerResolutionFn>("NtQueryTimerResolution");
  ULONG coarsest = 0, finest = 0, current = 0;
  if (!query || query(&coarsest, &finest, &current) < 0) return (std::chrono::nanoseconds::max)();
  return std::chrono::nanoseconds(std::int64_t(current) * 100);
#else
  timespec resolution{};
  if (clock_getres(CLOCK_MONOTONIC, &resolution) != 0) return (std::chrono::nanoseconds::max)();
  return std::chrono::seconds(resolution.tv_sec) + std::chrono::nanoseconds(resolution.tv_nsec);
#endif
}

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
