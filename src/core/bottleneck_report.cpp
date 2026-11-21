#include "xenon/core/bottleneck_report.hpp"

#include <algorithm>
#include <cstdio>

namespace xenon::core {

void BottleneckLedger::add_slow(const std::string& category, const std::string& name,
                                std::uint64_t count, double total_ms, double max_ms) {
  std::scoped_lock lock(mutex_);
  auto& item = slow_[{category, name}];
  item.category = category;
  item.name = name;
  item.count += count;
  item.total_ms += total_ms;
  item.max_ms = std::max(item.max_ms, max_ms);
}

void BottleneckLedger::add_hitch(double interval_ms, double expected_ms) {
  std::scoped_lock lock(mutex_);
  ++hitches_;
  hitch_excess_ms_ += std::max(0.0, interval_ms - expected_ms);
  hitch_max_ms_ = std::max(hitch_max_ms_, interval_ms);
}

std::vector<BottleneckItem> BottleneckLedger::items() const {
  std::scoped_lock lock(mutex_);
  std::vector<BottleneckItem> result;
  for (const auto& [key, item] : slow_) result.push_back(item);
  if (hitches_)
    result.push_back({"frame", "hitch frames (time over the median)", hitch_excess_ms_, hitches_,
                      hitch_max_ms_});
  return result;
}

std::uint64_t BottleneckLedger::hitches() const {
  std::scoped_lock lock(mutex_);
  return hitches_;
}

std::vector<BottleneckItem> rank_bottlenecks(std::vector<BottleneckItem> items,
                                             std::size_t limit) {
  std::erase_if(items, [](const BottleneckItem& item) { return !(item.total_ms > 0.0); });
  std::sort(items.begin(), items.end(), [](const BottleneckItem& a, const BottleneckItem& b) {
    if (a.total_ms != b.total_ms) return a.total_ms > b.total_ms;
    return a.category != b.category ? a.category < b.category : a.name < b.name;
  });
  if (items.size() > limit) items.resize(limit);
  return items;
}

std::string format_bottleneck_report(double run_seconds, std::uint64_t frames,
                                     const std::vector<BottleneckItem>& ranked,
                                     const std::vector<BottleneckRatio>& ratios) {
  std::string out;
  char line[320];
  std::snprintf(line, sizeof(line),
                "bottlenecks after %.1f s, %llu presented frames (times are summed over threads: "
                "shares of wall time can exceed 100%%)\n",
                run_seconds, static_cast<unsigned long long>(frames));
  out += line;
  std::snprintf(line, sizeof(line), "%4s  %-9s %-44s %12s %7s %9s %9s %10s\n", "rank", "category",
                "cost", "total ms", "% run", "ms/frame", "events", "max ms");
  out += line;
  const double wall_ms = run_seconds * 1000.0;
  int rank = 0;
  for (const auto& item : ranked) {
    ++rank;
    std::snprintf(line, sizeof(line), "%4d  %-9.9s %-44.44s %12.1f %6.1f%% %9.3f %9llu %10.2f\n",
                  rank, item.category.c_str(), item.name.c_str(), item.total_ms,
                  wall_ms > 0.0 ? 100.0 * item.total_ms / wall_ms : 0.0,
                  frames ? item.total_ms / static_cast<double>(frames) : 0.0,
                  static_cast<unsigned long long>(item.count), item.max_ms);
    out += line;
  }
  if (ranked.empty()) out += "  (nothing measurable yet)\n";
  if (!ratios.empty()) {
    out += "work avoided (saved / total):\n";
    for (const auto& ratio : ratios) {
      const auto total = ratio.saved + ratio.done;
      std::snprintf(line, sizeof(line), "  %-44.44s saved %12llu of %12llu (%5.1f%%)\n",
                    ratio.name.c_str(), static_cast<unsigned long long>(ratio.saved),
                    static_cast<unsigned long long>(total),
                    total ? 100.0 * static_cast<double>(ratio.saved) / static_cast<double>(total)
                          : 0.0);
      out += line;
    }
  }
  return out;
}

}  // namespace xenon::core
