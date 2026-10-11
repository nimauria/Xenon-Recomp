#pragma once

#include <cstdint>

namespace xenon::kernel {

// The guest-memory copy of a waitable object's signal state, for objects that
// live in guest memory: a DISPATCHER_HEADER's SignalState, used through the
// Ke* exports. On hardware that field is the object's state, so the host
// object keeps it current and adopts what the guest writes to it directly.
//
// The object calls both with its own mutex held: store() after every change
// to its state, load() when reconciling (see GuestStateSync). An
// implementation must not lock anything a thread may hold while it waits on
// or signals a kernel object.
class SignalStateMirror {
 public:
  virtual ~SignalStateMirror() = default;
  [[nodiscard]] virtual std::int32_t load() noexcept = 0;
  virtual void store(std::int32_t signal_state) noexcept = 0;
};

// Result of reconciling an object with its guest-memory copy.
enum class GuestStateSync : std::uint8_t {
  // The guest copy matched the object, or the object has no mirror.
  Unchanged,
  // The guest wrote a different, valid state directly; the object took it.
  // Like such a write on hardware, it wakes no waiter by itself: it is seen
  // by the next wait or signal.
  Adopted,
  // The guest wrote a state the object cannot hold (a semaphore count below
  // zero or above its limit); the object kept its own state.
  Rejected,
};

}  // namespace xenon::kernel
