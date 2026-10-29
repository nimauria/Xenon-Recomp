#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "xenon/cpu/types.hpp"

namespace xenon::memory {
class AddressSpace;
}

namespace xenon::kernel {

// KeTimeStampBundle, the xboxkrnl data export (ordinal 0xAD) titles read in
// place of calling a time function. 24 bytes, big-endian:
//   +0x00 u64 InterruptTime  100 ns units since boot
//   +0x08 u64 SystemTime     100 ns units since 1601 (FILETIME)
//   +0x10 u32 TickCount      milliseconds since boot
//   +0x14 u32 padding
// The console's clock interrupt rewrites it every millisecond, so code that
// polls TickCount sees time pass without making a kernel call.
struct TimeStampBundle {
  std::uint64_t interrupt_time{};
  std::uint64_t system_time{};
  std::uint32_t tick_count{};
};

inline constexpr std::uint32_t kTimeStampBundleSize = 24u;

// The bundle's values `now` for a console that booted at `boot`, with
// `system_time` as the wall clock in FILETIME units.
[[nodiscard]] TimeStampBundle time_stamp_bundle_at(std::chrono::steady_clock::time_point boot,
                                                   std::chrono::steady_clock::time_point now,
                                                   std::uint64_t system_time) noexcept;

// Stores `bundle` at `address` in the guest layout above. Throws
// memory::MemoryFault when the bundle is not writable guest memory.
void write_time_stamp_bundle(memory::AddressSpace& memory, cpu::GuestAddress address,
                             const TimeStampBundle& bundle);

// Keeps one guest KeTimeStampBundle current: a host thread rewrites it every
// `period` from the moment start() runs (the guest's boot) until stop().
class TimeStampBundleClock {
 public:
  TimeStampBundleClock() = default;
  ~TimeStampBundleClock();
  TimeStampBundleClock(const TimeStampBundleClock&) = delete;
  TimeStampBundleClock& operator=(const TimeStampBundleClock&) = delete;

  // Writes the bundle at once, then keeps it current. The clock holds
  // `memory` until stop(). Restarting resets boot to now. Returns false (and
  // stays stopped) when the bundle cannot be written.
  [[nodiscard]] bool start(std::shared_ptr<memory::AddressSpace> memory,
                           cpu::GuestAddress address,
                           std::chrono::microseconds period = std::chrono::milliseconds(1));
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept;

 private:
  void run(std::chrono::steady_clock::time_point boot);

  std::shared_ptr<memory::AddressSpace> memory_{};
  cpu::GuestAddress address_{};
  std::chrono::microseconds period_{};
  mutable std::mutex mutex_{};
  std::condition_variable wake_{};
  bool stopping_{false};
  std::thread thread_{};
};

}  // namespace xenon::kernel
