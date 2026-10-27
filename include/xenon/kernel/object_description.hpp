#pragma once

#include <cstdint>
#include <string>

#include "xenon/kernel/object.hpp"

namespace xenon::kernel {

// One line describing a waitable object for a stall report: its type, its
// current state (an event's signal and reset kind, a semaphore's count and
// limit, a mutant's owner) and the last guest signal noted on it, with its
// age against `now_ns` (steady_clock nanoseconds). For example:
//   "event (auto-reset) not signalled; last signalled by t2 (NtSetEvent from
//    0x821F4144) 3 ms ago, 120 signals"
[[nodiscard]] std::string describe_object_state(const KernelObject& object, std::int64_t now_ns);

}  // namespace xenon::kernel
