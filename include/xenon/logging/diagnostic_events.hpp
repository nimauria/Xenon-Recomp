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
#include <vector>

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

}  // namespace xenon::logging::events
