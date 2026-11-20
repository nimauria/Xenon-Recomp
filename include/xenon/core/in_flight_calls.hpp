#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "xenon/core/export_registry.hpp"

namespace xenon::core {

// Kernel calls guest threads have entered but not yet returned from. The
// completed-call trace cannot show a thread blocked forever in a wait, which
// is exactly what a stalled title looks like, so stall, stop and profiler
// reports read this.
//
// Every kernel call enters and leaves here, from every guest thread, so the
// write side takes no lock and touches no shared state: each host thread owns
// one slot and publishes into it under a sequence counter. Readers (reports,
// the sampling profiler) take a lock only against slot creation and skip a
// slot that changes while they copy it.
class InFlightCalls {
 public:
  struct Call {
    std::uint32_t thread_id{};
    std::uint32_t ordinal{};
    // Registry descriptors are immutable once the session is initialized, so
    // the pointer stays valid while the call is in flight (null = unknown).
    const ExportDescriptor* descriptor{};
    std::uint64_t lr{};
    std::array<std::uint64_t, 4> arguments{};
    std::chrono::steady_clock::time_point since{};
  };

  // Restores the call a thread was already inside when a kernel call runs
  // guest code that makes another kernel call.
  class Scope {
   public:
    Scope(InFlightCalls& calls, const Call& call);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    InFlightCalls* calls_;
    std::optional<Call> outer_;
  };

  InFlightCalls();
  ~InFlightCalls();
  InFlightCalls(const InFlightCalls&) = delete;
  InFlightCalls& operator=(const InFlightCalls&) = delete;

  // The calling host thread is now inside `call`; returns the call it was
  // already inside, to hand back to leave().
  [[nodiscard]] std::optional<Call> enter(const Call& call);
  void leave(const std::optional<Call>& outer);

  // Every call in flight, by guest thread id. Should two host threads report
  // the same guest thread, the most recently entered call is kept.
  [[nodiscard]] std::map<std::uint32_t, Call> snapshot() const;

 private:
  struct Slot;
  [[nodiscard]] Slot& slot_for_this_thread();
  static void publish(Slot& slot, const std::optional<Call>& call) noexcept;

  const std::uint64_t instance_id_;
  mutable std::mutex slots_mutex_{};
  std::vector<std::unique_ptr<Slot>> slots_{};
};

}  // namespace xenon::core
