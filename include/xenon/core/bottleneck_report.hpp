#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace xenon::core {

// The run's ranked bottlenecks (bottlenecks.log): every place the run spent
// time that a frame could be waiting on, ordered by total time, so one run
// yields the evidence for what to fix first. Items come from the GPU perf
// counters (ring decode, IR play, present, fence and backend waits, shader
// and pipeline builds, EDRAM moves, ...), the slow kernel events and the
// hitches the performance watch saw.
//
// Times are summed over threads, so shares of the run's wall time can add up
// to more than 100%: they say how much of the run a cost took, not how the
// run divides.
struct BottleneckItem {
  std::string category;  // "gpu", "kernel", "callback", "frame", ...
  std::string name;
  double total_ms{};
  std::uint64_t count{};  // 0 when the cost has no event count
  double max_ms{};        // 0 when unknown
};

// A cost in the form dedup/cache counters take: how often work was avoided.
struct BottleneckRatio {
  std::string name;
  std::uint64_t saved{};
  std::uint64_t done{};
};

// What the watch thread accumulates over a run: slow-event tallies by name
// and the hitches. Thread-safe.
class BottleneckLedger {
 public:
  void add_slow(const std::string& category, const std::string& name, std::uint64_t count,
                double total_ms, double max_ms);
  // A hitch: a frame of `interval_ms` where `expected_ms` was usual.
  void add_hitch(double interval_ms, double expected_ms);
  [[nodiscard]] std::vector<BottleneckItem> items() const;
  [[nodiscard]] std::uint64_t hitches() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::pair<std::string, std::string>, BottleneckItem> slow_;
  std::uint64_t hitches_{};
  double hitch_excess_ms_{};
  double hitch_max_ms_{};
};

// Items with no time are dropped; the rest are ordered by total time (ties
// by category, then name) and cut to `limit`.
[[nodiscard]] std::vector<BottleneckItem> rank_bottlenecks(std::vector<BottleneckItem> items,
                                                           std::size_t limit = 40);

// The file's text: header (run length, presented frames), the ranked list with
// each item's share of the run's wall time and per-frame cost, then the
// ratios.
[[nodiscard]] std::string format_bottleneck_report(double run_seconds, std::uint64_t frames,
                                                   const std::vector<BottleneckItem>& ranked,
                                                   const std::vector<BottleneckRatio>& ratios);

}  // namespace xenon::core
