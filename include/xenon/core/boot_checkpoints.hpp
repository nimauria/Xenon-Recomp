#pragma once

// Boot phase checkpoints (Part 15 of the AC6 Runtime Readiness / Platform
// Fidelity pass). A structured, ordered record of platform-level,
// game-agnostic milestones a session passes through while loading and
// running a title, so a stalled/crashed run makes it obvious where progress
// actually stopped - without hardcoding any game-state address or title-
// specific signal. Every checkpoint here is something Xenon itself can
// observe generically (a XEX finished loading, a guest thread was created,
// ...), never something only a specific game's own code would signal.
//
// Deliberately excludes title-visible milestones like "reached the title
// screen" or "mission load complete" - the pass's own instructions are
// explicit that those must never be hardcoded game-state detection in Xenon
// core. A future game module that wants to report a milestone of that kind
// has its own, separate channel (e.g. its own diagnostics/telemetry), not
// this one.

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

namespace xenon::core {

enum class BootCheckpoint : std::uint8_t {
  XexLoaded = 0,
  EntryStarted,
  FirstGuestThread,
  FirstFileOpen,
  FirstInputPoll,
  FirstAudioClient,
  FirstGpuSubmission,
  FirstShader,
  FirstResolve,
  FirstPresent,
  FirstVblank,
  ProfileReady,
  SaveEnumeration,
  // Sentinel - keep last; used only to size internal storage.
  Count,
};

[[nodiscard]] std::string_view to_string(BootCheckpoint checkpoint) noexcept;

// Idempotent, thread-safe, insertion-order-preserving record of which
// checkpoints a session has reached. Reaching the same checkpoint twice
// (e.g. FirstGuestThread for a session that creates many guest threads) is
// a no-op after the first call - this tracks progress milestones, not a
// running count (see GpuPerformanceCounters/GpuUnsupportedCounters for
// counters).
class BootCheckpointTracker {
 public:
  // Returns true the first time `checkpoint` is reached (so a caller can
  // decide whether to log it), false on every later call.
  bool reach(BootCheckpoint checkpoint);
  [[nodiscard]] bool reached(BootCheckpoint checkpoint) const noexcept;
  // In the order they were actually reached (not enum declaration order) -
  // this is what makes "where did it stop" legible: the last entry is the
  // furthest real progress made.
  [[nodiscard]] std::vector<BootCheckpoint> reached_in_order() const;
  void reset() noexcept;

 private:
  mutable std::mutex mutex_{};
  std::array<bool, static_cast<std::size_t>(BootCheckpoint::Count)> reached_{};
  std::vector<BootCheckpoint> order_{};
};

}  // namespace xenon::core
