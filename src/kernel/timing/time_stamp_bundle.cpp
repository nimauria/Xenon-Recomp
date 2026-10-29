#include "xenon/kernel/time_stamp_bundle.hpp"

#include <string>

#include "xenon/kernel/time.hpp"
#include "xenon/logging/logger.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/fault.hpp"

namespace xenon::kernel {

TimeStampBundle time_stamp_bundle_at(std::chrono::steady_clock::time_point boot,
                                     std::chrono::steady_clock::time_point now,
                                     std::uint64_t system_time) noexcept {
  const auto uptime = now > boot ? now - boot : std::chrono::steady_clock::duration::zero();
  TimeStampBundle bundle{};
  bundle.interrupt_time = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>>(
          uptime)
          .count());
  // TickCount is a 32-bit millisecond counter and wraps like the console's.
  bundle.tick_count = static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(uptime).count());
  bundle.system_time = system_time;
  return bundle;
}

void write_time_stamp_bundle(memory::AddressSpace& memory, cpu::GuestAddress address,
                             const TimeStampBundle& bundle) {
  memory.write64_be(address + 0x00u, bundle.interrupt_time);
  memory.write64_be(address + 0x08u, bundle.system_time);
  memory.write32_be(address + 0x10u, bundle.tick_count);
  memory.write32_be(address + 0x14u, 0u);
}

TimeStampBundleClock::~TimeStampBundleClock() { stop(); }

bool TimeStampBundleClock::start(std::shared_ptr<memory::AddressSpace> memory,
                                 cpu::GuestAddress address, std::chrono::microseconds period) {
  stop();
  if (!memory || period <= std::chrono::microseconds::zero()) return false;
  const auto boot = std::chrono::steady_clock::now();
  try {
    write_time_stamp_bundle(*memory, address,
                            time_stamp_bundle_at(boot, boot, TimeServices::system_time()));
  } catch (const memory::MemoryFault&) {
    return false;
  }
  memory_ = std::move(memory);
  address_ = address;
  period_ = period;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = false;
  }
  thread_ = std::thread([this, boot] { run(boot); });
  return true;
}

void TimeStampBundleClock::stop() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
  memory_.reset();
}

bool TimeStampBundleClock::running() const noexcept { return thread_.joinable(); }

void TimeStampBundleClock::run(std::chrono::steady_clock::time_point boot) {
  // At the host's default 15.6 ms timer step the bundle would advance in
  // 15 ms jumps instead of the console's 1 ms.
  const HostTimerResolution timer_resolution{};
  auto next = boot + period_;
  std::unique_lock<std::mutex> lock(mutex_);
  while (!wake_.wait_until(lock, next, [this] { return stopping_; })) {
    const auto now = std::chrono::steady_clock::now();
    try {
      write_time_stamp_bundle(*memory_, address_,
                              time_stamp_bundle_at(boot, now, TimeServices::system_time()));
    } catch (const memory::MemoryFault& fault) {
      // The bundle's page is gone: the session is tearing guest memory down.
      logging::Logger::instance().log_if_enabled(
          logging::Level::Warning, "kernel", [what = std::string(fault.what())] {
            return "KeTimeStampBundle clock stopped: " + what;
          });
      return;
    }
    next += period_;
    // A host stall must not turn into a burst of catch-up writes.
    if (next < now) next = now + period_;
  }
}

}  // namespace xenon::kernel
