#pragma once

// Activity trace: a record of everything the title and Xenon do while it
// runs - every kernel/XAM call with its arguments, result and response time,
// and each GPU draw, resolve, EDRAM transfer, texture decode, shader compile,
// ring submission, present, vsync and audio callback - one stream per thread,
// condensed so a long run stays readable:
//
//   +12.345678s T4 main   xbox KeWaitForSingleObject(r3=0x1DC r4=0x0) -> 0x0  16.20 ms
//   +12.36s..+14.61s T4 main  x812 cycle of 3, 3.1 ms/cycle:
//       | xbox KeWaitForSingleObject(r3=0x1DC r4=0x0) -> 0x0  avg 2.70 ms, max 16.20 ms
//       | xbox KeSetEvent(r3=0x1E0 r4=0x1) -> 0x0  avg 0.01 ms, max 0.02 ms
//       | gpu draw(prim=0x4 count=0x24 vs=0x...)  avg 0.30 ms, max 1.10 ms
//
// A run of one event, or a cycle of up to kMaxCycle events repeating back to
// back, becomes one entry with its repeat count and the average and maximum
// time of each step; anything else is written as it happened. A cycle still
// repeating after a second is written as it stands and continues
// ("continues"), so the file stays in time order. At the end the summary
// lists each event's count and total/average/maximum time, overall and per
// thread, and each thread's traced time against its lifetime.
//
// Off unless XENON_ACTIVITY_TRACE=1 is set (or set_activity_trace_enabled()).
// Files: activity_trace.log and activity_summary.txt in probe_log_directory().

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace xenon::logging {

[[nodiscard]] bool activity_trace_enabled() noexcept;
void set_activity_trace_enabled(bool enabled) noexcept;

// An event kind: a category ("xbox", "gpu", "audio", ...), a name, and labels
// for up to four numeric arguments. `has_result` adds "-> value".
struct ActivityKind {
  std::string category;
  std::string name;
  std::array<std::string, 4> labels{};
  bool has_result{};
};

// Registers (or finds) an event kind and returns its key. Interning takes a
// lock; hot paths keep the key in a static or cache it per call site.
[[nodiscard]] std::uint32_t intern_activity(const ActivityKind& kind);
[[nodiscard]] const ActivityKind& activity_kind(std::uint32_t key);

struct ActivityEvent {
  std::uint32_t key{};
  std::array<std::uint64_t, 4> arguments{};
  std::uint64_t result{};
  std::uint64_t start_ns{};     // since the trace started
  std::uint64_t duration_ns{};
  std::string text{};           // optional, for rare events (a file path)
};

// Nanoseconds since the trace started (the steady clock).
[[nodiscard]] std::uint64_t activity_now_ns() noexcept;

// Records one event on the calling thread's stream. A no-op while off.
void record_activity(std::uint32_t key, const std::array<std::uint64_t, 4>& arguments,
                     std::uint64_t result, std::uint64_t start_ns, std::uint64_t duration_ns,
                     std::string text = {});

// Names the calling thread's stream ("T4 main", "gpu pump"). Later names
// replace earlier ones; a stream without one is named by its host thread.
void set_activity_thread_label(std::string label);

// Times a scope and records it when the scope ends. The arguments may be set
// while it runs.
class ActivityScope {
 public:
  ActivityScope(std::uint32_t key, std::array<std::uint64_t, 4> arguments = {}) noexcept;
  ~ActivityScope();
  ActivityScope(const ActivityScope&) = delete;
  ActivityScope& operator=(const ActivityScope&) = delete;
  std::array<std::uint64_t, 4> arguments{};
  std::uint64_t result{};
  std::string text{};

 private:
  std::uint32_t key_{};
  std::uint64_t start_{};
  bool active_{};
};

// Writes every stream's pending entries and the summary, and flushes the
// file. Safe to call more than once; recording may continue afterwards.
void flush_activity_trace();
// Rewrites the summary and flushes the trace file without closing any
// stream's open cycle; for periodic snapshots while the title runs.
void write_activity_summary();

// The condensing step on its own, for tests and offline use: events in, text
// lines out (through `sink`), in the order they are final.
class ActivityCondenser {
 public:
  static constexpr std::size_t kMaxCycle = 8;
  static constexpr std::size_t kWindow = 2 * kMaxCycle;
  // How long a repeating cycle runs before it is written and continues.
  static constexpr std::uint64_t kProgressNs = 1'000'000'000ull;

  ActivityCondenser(std::string label, std::function<void(const std::string&)> sink);
  void add(ActivityEvent event);
  // Writes the open cycle (if any) and every held event.
  void flush();

  struct Step {
    ActivityEvent first{};       // its first occurrence, for the arguments
    std::uint64_t count{};
    std::uint64_t total_ns{};
    std::uint64_t max_ns{};
  };

 private:
  void emit_single(const ActivityEvent& event);
  void close_cycle(bool continues);
  [[nodiscard]] std::string describe(const ActivityEvent& event) const;

  std::string label_;
  std::function<void(const std::string&)> sink_;
  std::vector<ActivityEvent> pending_;
  // The open cycle.
  std::vector<Step> cycle_;
  std::size_t position_{};
  std::uint64_t repeats_{};
  std::uint64_t cycle_start_ns_{};
  std::uint64_t cycle_end_ns_{};
  std::uint64_t cycle_emitted_ns_{};
  bool continued_{};
};

// Formats `ns` as milliseconds ("16.20 ms") or microseconds below 0.1 ms.
[[nodiscard]] std::string format_activity_duration(std::uint64_t ns);

}  // namespace xenon::logging
