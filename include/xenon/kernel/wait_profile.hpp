#pragma once

// Wait attribution: where guest threads block, on what, for how long, and
// who released them. Every wait that registers (one that is not satisfied at
// once) is recorded when it ends, keyed by the waiting thread, the object
// that ended it (the one granted, or the first when it timed out or was
// interrupted) and that object's last signal (KernelObject::last_signal():
// the releasing guest thread, its caller and the export it signalled
// through). The report ranks them by blocked time, so a stall names its
// cause, duration and the dependency that ended it.
//
// Off until enabled (XENON_WAIT_PROFILE=1, or with XENON_HOST_PROFILE - see
// XenonSession::start_guest_profile()); recording then costs one short lock
// per blocking wait, nothing for waits satisfied at once.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xenon/kernel/io_types.hpp"

namespace xenon::kernel {

class KernelObject;
enum class WaitResult : std::uint32_t;

[[nodiscard]] bool wait_profile_enabled() noexcept;
void set_wait_profile_enabled(bool enabled) noexcept;
void reset_wait_profile();

// Records one blocking wait of `blocked_ns` by guest thread `thread_id` that
// `object` ended with `result`; `spin_hit` when the wait was granted while it
// spun, before it blocked its host thread.
void note_blocking_wait(std::uint32_t thread_id, const KernelObject& object, WaitResult result,
                        std::uint64_t blocked_ns, bool spin_hit = false);

// Pre-block spinning, over every registered wait (one not satisfied at
// once) since the last reset_wait_profile(); counted whether or not the
// profile is enabled, as each costs one relaxed add on a wait that already
// registered.
struct WaitSpinCounters {
  std::uint64_t registered{};  // waits that registered
  std::uint64_t spins{};       // of those, waits that spun before blocking
  std::uint64_t spin_hits{};   // granted while spinning: never blocked
  std::uint64_t blocks{};      // waits that blocked their host thread
  std::uint64_t spin_ns{};     // host time spent spinning
};
[[nodiscard]] WaitSpinCounters wait_spin_counters() noexcept;
// Records one registered wait (wait.cpp).
void note_registered_wait(bool spun, bool spin_hit, bool blocked, std::uint64_t spin_ns) noexcept;

struct WaitProfileEntry {
  std::uint32_t thread_id{};
  std::uint64_t object_id{};
  ObjectType object_type{ObjectType::Unknown};
  // The object's last signal when the wait ended (0 / null: never signalled).
  std::uint32_t signaler_thread_id{};
  std::uint32_t signaler_caller{};
  const char* signal_source{};
  std::uint64_t waits{};
  std::uint64_t timeouts{};  // ended without the object (timeout, alert, APC, terminate)
  std::uint64_t spin_hits{};  // granted while spinning, before blocking
  std::uint64_t total_ns{};
  std::uint64_t max_ns{};
};

// Every recorded (thread, object, signaler) entry, most blocked time first.
[[nodiscard]] std::vector<WaitProfileEntry> wait_profile_entries();
// The report: the spin counters, then per thread its total blocked time and
// its `top_per_thread` largest entries.
[[nodiscard]] std::string wait_profile_report(std::size_t top_per_thread = 12);

}  // namespace xenon::kernel
