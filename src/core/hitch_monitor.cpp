#include "xenon/core/hitch_monitor.hpp"

#include <algorithm>
#include <unordered_map>

namespace xenon::core {

HitchMonitor::HitchMonitor() : HitchMonitor(Options{}) {}

HitchMonitor::HitchMonitor(Options options)
    : options_(options), samples_(std::max<std::size_t>(options.sample_capacity, 1u)) {}

HitchMonitor::FrameVerdict HitchMonitor::note_frame(std::uint64_t time_ns) {
  std::scoped_lock lock(mutex_);
  FrameVerdict verdict{};
  if (last_frame_ns_ == 0u || time_ns <= last_frame_ns_) {
    last_frame_ns_ = time_ns;
    return verdict;
  }
  verdict.interval_ms = static_cast<double>(time_ns - last_frame_ns_) / 1e6;
  last_frame_ns_ = time_ns;
  if (intervals_.size() >= options_.min_history) {
    std::vector<double> sorted(intervals_.begin(), intervals_.end());
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    verdict.expected_ms = sorted[sorted.size() / 2];
    const auto margin = std::max(options_.hitch_margin_ms,
                                 verdict.expected_ms * options_.hitch_margin_fraction);
    verdict.hitch = verdict.interval_ms > verdict.expected_ms + margin;
  }
  intervals_.push_back(verdict.interval_ms);
  while (intervals_.size() > options_.median_window) intervals_.pop_front();
  return verdict;
}

void HitchMonitor::add_sample(const Sample& sample) {
  std::scoped_lock lock(mutex_);
  samples_[next_sample_] = sample;
  if (++next_sample_ == samples_.size()) {
    next_sample_ = 0;
    samples_wrapped_ = true;
  }
}

std::vector<HitchMonitor::ThreadActivity> HitchMonitor::activity_between(
    std::uint64_t from, std::uint64_t to, std::size_t top_n,
    const std::function<std::uint32_t(std::uint32_t)>& fold) const {
  std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, std::uint32_t>> counts;
  std::unordered_map<std::uint32_t, std::uint32_t> totals;
  {
    std::scoped_lock lock(mutex_);
    const auto count = samples_wrapped_ ? samples_.size() : next_sample_;
    for (std::size_t i = 0; i < count; ++i) {
      const auto& sample = samples_[i];
      if (sample.time_ns < from || sample.time_ns >= to) continue;
      ++totals[sample.thread];
      ++counts[sample.thread][sample.pc];
    }
  }
  std::vector<ThreadActivity> result;
  for (const auto& [thread, by_pc] : counts) {
    ThreadActivity activity{};
    activity.thread = thread;
    activity.samples = totals[thread];
    std::unordered_map<std::uint32_t, std::uint32_t> folded;
    for (const auto& [pc, n] : by_pc) folded[fold ? fold(pc) : pc] += n;
    activity.top.assign(folded.begin(), folded.end());
    std::sort(activity.top.begin(), activity.top.end(), [](const auto& a, const auto& b) {
      return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (activity.top.size() > top_n) activity.top.resize(top_n);
    result.push_back(std::move(activity));
  }
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return a.samples != b.samples ? a.samples > b.samples : a.thread < b.thread;
  });
  return result;
}

bool HitchMonitor::note_slow(const std::string& category, const std::string& name,
                             std::uint64_t duration_ns) {
  std::scoped_lock lock(mutex_);
  const auto key = std::make_pair(category, name);
  auto& tally = tallies_[key];
  if (tally.count == 0u) {
    tally.category = category;
    tally.name = name;
  }
  ++tally.count;
  tally.total_ns += duration_ns;
  tally.max_ns = std::max(tally.max_ns, duration_ns);
  auto& detailed = detailed_[key];
  if (detailed >= options_.detailed_per_name) return false;
  ++detailed;
  return true;
}

std::vector<HitchMonitor::SlowTally> HitchMonitor::take_slow_tallies() {
  std::vector<SlowTally> result;
  {
    std::scoped_lock lock(mutex_);
    for (auto& [key, tally] : tallies_) result.push_back(std::move(tally));
    tallies_.clear();
  }
  std::sort(result.begin(), result.end(),
            [](const auto& a, const auto& b) { return a.total_ns > b.total_ns; });
  return result;
}

}  // namespace xenon::core
