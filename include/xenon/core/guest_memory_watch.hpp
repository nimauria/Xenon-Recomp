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

// A watched 32-bit word: the word at `address`, or, with `offsets`, the end of
// a pointer chain - read the word at `address`, then for each offset read the
// word at (previous value + offset). Written "0x8293B930->0x8->0x0": the word
// at offset 0 of the object whose pointer sits at offset 8 of the object
// 0x8293B930 points to. A null or unreadable link makes the value unreadable.
struct WatchTarget {
  cpu::GuestAddress address{};
  std::vector<std::uint32_t> offsets{};
  WatchTarget() = default;
  WatchTarget(cpu::GuestAddress plain) : address(plain) {}  // NOLINT: a plain address is a target
  WatchTarget(cpu::GuestAddress base, std::vector<std::uint32_t> chain)
      : address(base), offsets(std::move(chain)) {}
  friend bool operator==(const WatchTarget&, const WatchTarget&) = default;
  friend auto operator<=>(const WatchTarget&, const WatchTarget&) = default;
};

// Parses "0x8293B930", "2191220416" or "0x8293B930->0x8->0x0" (each part a
// C-style integer, decimal or 0x hex, at most 32 bits). Empty when malformed.
[[nodiscard]] std::optional<WatchTarget> parse_watch_target(std::string_view text);
[[nodiscard]] std::string format_watch_target(const WatchTarget& target);

struct WatchRecord {
  std::uint64_t sequence{};
  std::int64_t microseconds{};  // since the watch was constructed
  cpu::GuestAddress address{};
  std::vector<std::uint32_t> offsets{};  // the target's pointer chain, if any
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
  GuestMemoryWatch(std::vector<WatchTarget> targets, std::size_t capacity);

  // Replaces the watched set and clears all history and baselines.
  void configure(std::vector<WatchTarget> targets, std::size_t capacity);

  [[nodiscard]] bool active() const noexcept { return active_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::vector<WatchTarget> targets() const;

  // Reads every watched word through `reader` and appends a record for each one
  // whose value differs from the previous sample. Returns the records added by this
  // call (also retained in the history). Thread-safe.
  std::vector<WatchRecord> sample(const Reader& reader, const WatchObserver& observer);

  // Oldest-to-newest.
  [[nodiscard]] std::vector<WatchRecord> history() const;
  [[nodiscard]] std::uint64_t total_changes() const;
  // Last value seen for `target` (nullopt if never readable or not watched).
  [[nodiscard]] std::optional<std::uint32_t> last_value(const WatchTarget& target) const;

  [[nodiscard]] static std::string format(const WatchRecord& record);

 private:
  struct Slot {
    WatchTarget target{};
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
