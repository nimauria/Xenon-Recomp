#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace xenon::core {

// Performance watch for a running title: decides which presented frames are
// hitches, keeps a short history of what every guest thread was executing
// (periodic program-counter samples), and tallies slow events (a kernel call,
// a callback, a present) by name. The session turns a hitch into a report -
// hitches.log - from these and its own traces, so a hitch or a slow stretch
// leaves the data that explains it.
//
// Times are steady-clock nanoseconds. Thread-safe.
class HitchMonitor {
 public:
  struct Options {
    // A frame is a hitch when its interval exceeds the recent median by this
    // much, or by this fraction of the median, whichever is larger.
    double hitch_margin_ms = 8.0;
    double hitch_margin_fraction = 0.25;
    // Frames of history the median is taken over, and how many it needs.
    std::size_t median_window = 31;
    std::size_t min_history = 8;
    // Program-counter samples kept (all threads together).
    std::size_t sample_capacity = 32768;
    // Full detail lines per slow-event name before it is only tallied.
    std::uint32_t detailed_per_name = 10;
  };

  struct Sample {
    std::uint64_t time_ns{};
    std::uint32_t thread{};
    std::uint32_t pc{};
  };

  struct FrameVerdict {
    double interval_ms{};
    double expected_ms{};  // the recent median; 0 until there is history
    bool hitch{};
  };

  struct ThreadActivity {
    std::uint32_t thread{};
    std::uint32_t samples{};
    // (code address after folding, samples), most samples first.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> top;
  };

  struct SlowTally {
    std::string category;
    std::string name;
    std::uint64_t count{};
    std::uint64_t total_ns{};
    std::uint64_t max_ns{};
  };

  HitchMonitor();
  explicit HitchMonitor(Options options);

  [[nodiscard]] const Options& options() const noexcept { return options_; }

  // A presented frame at `time_ns`. The first frame has no interval.
  FrameVerdict note_frame(std::uint64_t time_ns);

  void add_sample(const Sample& sample);
  // What each thread ran in [from, to): samples per thread and the `top_n`
  // busiest addresses after `fold` (e.g. to the containing function), busiest
  // threads first. The session samples a thread that is inside a kernel call
  // at address 0.
  [[nodiscard]] std::vector<ThreadActivity> activity_between(
      std::uint64_t from, std::uint64_t to, std::size_t top_n,
      const std::function<std::uint32_t(std::uint32_t)>& fold = {}) const;

  // A slow event; true when it should be written out in full (the first few
  // of each name), false when it is only tallied.
  bool note_slow(const std::string& category, const std::string& name,
                 std::uint64_t duration_ns);
  // The tallies since the last call, by total time, and resets them.
  [[nodiscard]] std::vector<SlowTally> take_slow_tallies();

 private:
  const Options options_;
  mutable std::mutex mutex_;
  std::deque<double> intervals_;
  std::uint64_t last_frame_ns_{};
  std::vector<Sample> samples_;
  std::size_t next_sample_{};
  bool samples_wrapped_{};
  std::map<std::pair<std::string, std::string>, SlowTally> tallies_;
  std::map<std::pair<std::string, std::string>, std::uint32_t> detailed_;
};

}  // namespace xenon::core
