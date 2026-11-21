// bottlenecks.log: items are ranked by total time with empty ones dropped, the
// ledger folds slow tallies and hitches, and the text carries rank, share of
// the run and per-frame cost.

#include <cassert>
#include <string>

#include "xenon/core/bottleneck_report.hpp"

using namespace xenon::core;

int main() {
  BottleneckLedger ledger;
  ledger.add_slow("kernel", "NtWaitForSingleObjectEx", 3, 30.0, 20.0);
  ledger.add_slow("kernel", "NtWaitForSingleObjectEx", 2, 25.0, 40.0);  // folds by name
  ledger.add_hitch(60.0, 33.0);
  ledger.add_hitch(50.0, 33.0);
  assert(ledger.hitches() == 2u);
  auto items = ledger.items();
  assert(items.size() == 2u);

  items.push_back({"gpu", "fence wait", 500.0, 10, 0.0});
  items.push_back({"gpu", "unused", 0.0, 0, 0.0});          // dropped: no time
  items.push_back({"gpu", "tie a", 55.0, 1, 0.0});
  const auto ranked = rank_bottlenecks(items);
  assert(ranked.size() == 4u);
  assert(ranked[0].name == "fence wait");
  // 55 ms: the folded wait (55) ties "tie a" (55) - ordered by category, so gpu first.
  assert(ranked[1].name == "tie a" && ranked[2].name == "NtWaitForSingleObjectEx");
  assert(ranked[2].count == 5u && ranked[2].total_ms == 55.0 && ranked[2].max_ms == 40.0);
  assert(ranked[3].category == "frame" && ranked[3].total_ms == 27.0 + 17.0);
  assert(rank_bottlenecks(items, 2).size() == 2u);

  const auto text = format_bottleneck_report(10.0, 300, ranked,
                                             {{"constants reused", 75, 25}, {"none", 0, 0}});
  assert(text.find("bottlenecks after 10.0 s, 300 presented frames") == 0u);
  const auto fence = text.find("fence wait");
  const auto wait = text.find("NtWaitForSingleObjectEx");
  assert(fence != std::string::npos && wait != std::string::npos && fence < wait);
  assert(text.find("5.0%") != std::string::npos);        // 500 ms of 10 s
  assert(text.find("1.667") != std::string::npos);       // 500 ms / 300 frames
  assert(text.find("( 75.0%)") != std::string::npos);     // 75 of 100 saved
  assert(text.find("(  0.0%)") != std::string::npos);
  assert(format_bottleneck_report(1.0, 0, {}, {}).find("nothing measurable") != std::string::npos);
  return 0;
}
