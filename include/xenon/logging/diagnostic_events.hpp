#pragma once

// Bounded, structured runtime diagnostic events.
//
// A process-wide ring of fixed-size records that guest-facing code can write
// from any thread without blocking and without file I/O. It replaces the
// unbounded `*_diag.log` investigation probes for high-volume paths (kernel
// waits and signals first). Recording is off by default; while off,
// record() costs one relaxed atomic load. Set XENON_DIAG_EVENTS=1 in the
// environment, or call set_enabled(true), to record.
//
// The ring keeps the most recent kCapacity events. Older events are
// overwritten, never blocked on; recorded() minus the snapshot size says how
// many were lost.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xenon/logging/failure_category.hpp"

namespace xenon::logging::events {

inline constexpr std::size_t kCapacity = std::size_t{1} << 16;

enum class EventKind : std::uint16_t {
  // A guest thread starts waiting. value: timeout in milliseconds, or
  // 0xFFFFFFFF for an infinite wait.
  WaitBegin = 1,
  // The wait returned. value: the NTSTATUS-style result.
  WaitEnd = 2,
  // A guest thread signalled an object. value: operation-specific
  // (for example the previous signal state).
  Signal = 3,
  // A guest thread took a spin lock or critical section (guest_address).
  // value: milliseconds spent acquiring it.
  LockAcquired = 4,
  // A guest thread released one. value: operation-specific (the critical
  // section's lock count after the release).
  LockReleased = 5,
  // The GPU command processor parked on an unsatisfied WAIT_REG_MEM, recorded
  // once per fence rather than on every retried drain. guest_address: the
  // polled physical address (with its endian bits) or register index;
  // value: the reference it waits for.
  GpuWaitParked = 6,
  // That wait was satisfied. value: the value that satisfied it.
  GpuWaitResumed = 7,
  // A Ke* call found that guest code had written an object's
  // DISPATCHER_HEADER SignalState directly since the kernel last wrote it,
  // and the object took that state. guest_address: the header; value: the
  // SignalState the guest wrote.
  GuestStateAdopted = 8,
  // As GuestStateAdopted, but the value cannot be the object's state (a
  // semaphore count outside 0 to its limit), so the object kept its own.
  GuestStateRejected = 9,
  // A pump thread entered a guest callback (an interrupt or audio render
  // callback). guest_address: the callback's entry; source: its kind, such
  // as "vsync interrupt".
  CallbackBegin = 10,
  // It returned. value: its duration in milliseconds.
  CallbackEnd = 11,
  // RtlRaiseException with an exception Xenon cannot dispatch (it has no
  // guest SEH unwinder); the call returned to the guest. guest_address: the
  // EXCEPTION_RECORD; value: the exception code.
  ExceptionNotDispatched = 12,
};

struct Event {
  // Assigned by record(): a process-wide total order of recording.
  std::uint64_t sequence{};
  // Assigned by record(): host monotonic clock (std::chrono::steady_clock),
  // nanoseconds. Not guest time.
  std::int64_t host_time_ns{};
  std::uint32_t guest_thread_id{};
  EventKind kind{};
  // kernel::KernelObject::object_id(), or 0 when the event has no object.
  std::uint64_t object_id{};
  // Guest address of the object (dispatcher header) or a handle value.
  std::uint32_t guest_address{};
  std::uint32_t value{};
  // The guest return address (LR) of the export call that recorded the
  // event, which identifies the calling guest code; 0 when there is none.
  std::uint32_t guest_caller{};
  // The operation that recorded the event, such as "KeWaitForSingleObject".
  // Must point to a string with static storage duration.
  const char* source{};
};

[[nodiscard]] bool enabled() noexcept;
void set_enabled(bool enabled) noexcept;

// Records `event` (its sequence and host_time_ns are assigned here). Never
// blocks on I/O or readers. A no-op while recording is off.
void record(Event event) noexcept;

// The retained events recorded since the last clear(), oldest first. Safe to
// call while other threads record; an event being written at that moment is
// left out rather than returned half-written.
[[nodiscard]] std::vector<Event> snapshot();

// Events recorded since the last clear(), including any since overwritten.
[[nodiscard]] std::uint64_t recorded() noexcept;

// Forgets every event recorded so far. Safe alongside record().
void clear() noexcept;

// A guest thread whose most recent wait in a snapshot began and has not
// ended: one entry per object it waits on (a multi-object wait gives one per
// object).
struct BlockedWait {
  std::uint32_t guest_thread_id{};
  std::uint64_t object_id{};
  std::uint32_t guest_address{};
  const char* source{};
  std::uint32_t guest_caller{};
  std::int64_t waiting_since_ns{};
  std::uint32_t timeout_ms{};
  // The most recent Signal of the object in the snapshot, before or after
  // the wait began. Absent when the snapshot holds none (it may have been
  // overwritten, or the object was never signalled through a recorded path).
  std::optional<Event> last_signal{};
  // True when that signal came after the wait began: a wake-up the waiter did
  // not receive, or a signal another waiter consumed.
  bool signalled_after_wait_began{};
  // DeadlockCandidate for a wait with no timeout; empty for a bounded wait,
  // which will end by itself.
  std::optional<FailureCategory> category{};
};

// Derives the waits still open at the end of `events` (snapshot() order).
// A thread whose WaitBegin was overwritten before the snapshot is not listed.
[[nodiscard]] std::vector<BlockedWait> blocked_waits(std::span<const Event> events);

// The guest callbacks still running at the end of `events`: per thread, a
// CallbackBegin with no CallbackEnd after it. A thread listed here and in
// blocked_waits() is a pump blocked inside a guest callback.
[[nodiscard]] std::vector<Event> open_callbacks(std::span<const Event> events);

// The GPU wait still parked at the end of `events`, if the last GpuWaitParked
// has no GpuWaitResumed after it.
[[nodiscard]] std::optional<Event> parked_gpu_wait(std::span<const Event> events);

// One line per blocked wait, for stop reports. `now_ns` is the host
// monotonic time the report describes.
[[nodiscard]] std::string format_blocked_waits(std::span<const BlockedWait> waits,
                                               std::int64_t now_ns);

}  // namespace xenon::logging::events
