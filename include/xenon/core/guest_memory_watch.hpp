#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/cpu/types.hpp"

namespace xenon::core {

// Where in the guest/host handoff a watched word was seen to change. The value is
// only ever sampled at these boundaries (no per-store hook), so a change is
// attributed to "somewhere since the previous sample" - the phase and thread say
// which observer noticed it, not which instruction stored it.
enum class WatchPhase : std::uint8_t {
  // Background host thread polling on a timer; independent of any guest thread.
  Poll,
  // A guest thread is about to enter an xboxkrnl/xam export.
  BeforeKernelCall,
  // A guest thread just returned from an export (a store made by the export itself
  // or by another thread during the call shows up here).
  AfterKernelCall,
  // A guest thread crossed a compiled-function boundary in the dispatch loop.
  DispatchBoundary,
};

[[nodiscard]] std::string_view watch_phase_name(WatchPhase phase) noexcept;

// Who is sampling and what they were doing.
struct WatchObserver {
  WatchPhase phase{WatchPhase::Poll};
  std::uint32_t thread_id{};  // 0 = not a guest thread (poll thread, test harness).
  cpu::GuestAddress cia{};
  cpu::GuestAddress nia{};
  std::uint64_t lr{};
  // Free-form context, e.g. the export being entered/left or the guest threads that
  // were executing when the poll thread noticed the change.
  std::string note{};
  // Optional lazily-built extra context, appended to `note` only when a change is
  // actually recorded. Invoked with no GuestMemoryWatch lock held, so it may take
  // other locks. Not retained in the stored record.
  std::function<std::string()> describe{};
};

struct WatchRecord {
  std::uint64_t sequence{};
  std::int64_t microseconds{};  // since the watch was constructed
  cpu::GuestAddress address{};
  // nullopt = the word could not be read (unmapped/uncommitted) at that sample.
  std::optional<std::uint32_t> old_value{};
  std::optional<std::uint32_t> new_value{};
  bool initial{};  // First successful observation, not a change.
  WatchObserver observer{};
};

// Optional, title-agnostic history of 32-bit guest words. Disabled (and free apart
// from one relaxed atomic load) when constructed with no addresses. Records only
// changes between samples, in a bounded ring; the oldest records are dropped first
// but `total_changes()` keeps counting.
class GuestMemoryWatch {
 public:
  using Reader = std::function<std::optional<std::uint32_t>(cpu::GuestAddress)>;
  static constexpr std::size_t kDefaultCapacity = 4096;

  GuestMemoryWatch() = default;
  GuestMemoryWatch(std::vector<cpu::GuestAddress> addresses, std::size_t capacity);

  // Replaces the watched set and clears all history and baselines.
  void configure(std::vector<cpu::GuestAddress> addresses, std::size_t capacity);

  [[nodiscard]] bool active() const noexcept { return active_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::vector<cpu::GuestAddress> addresses() const;

  // Reads every watched word through `reader` and appends a record for each one
  // whose value differs from the previous sample. Returns the records added by this
  // call (also retained in the history). Thread-safe.
  std::vector<WatchRecord> sample(const Reader& reader, const WatchObserver& observer);

  // Oldest-to-newest.
  [[nodiscard]] std::vector<WatchRecord> history() const;
  [[nodiscard]] std::uint64_t total_changes() const;
  // Last value seen for `address` (nullopt if never readable or not watched).
  [[nodiscard]] std::optional<std::uint32_t> last_value(cpu::GuestAddress address) const;

  [[nodiscard]] static std::string format(const WatchRecord& record);

 private:
  struct Slot {
    cpu::GuestAddress address{};
    bool seen{};
    std::optional<std::uint32_t> value{};
  };

  std::atomic<bool> active_{false};
  mutable std::mutex mutex_{};
  std::vector<Slot> slots_{};
  std::vector<WatchRecord> ring_{};
  std::size_t capacity_{kDefaultCapacity};
  std::size_t next_index_{};
  std::size_t size_{};
  std::uint64_t next_sequence_{1};
  std::uint64_t total_changes_{};
  std::chrono::steady_clock::time_point epoch_{std::chrono::steady_clock::now()};
};

}  // namespace xenon::core
