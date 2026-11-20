// HitchMonitor: a frame well over the recent median is a hitch and steady
// frames are not; samples in a window fold by function per thread; slow
// events are written in full for the first few of a name, then tallied.

#include <cassert>
#include <cstdint>

#include "xenon/core/hitch_monitor.hpp"

using xenon::core::HitchMonitor;

namespace {

constexpr std::uint64_t kMs = 1'000'000u;

void test_frame_hitches() {
  HitchMonitor monitor;
  std::uint64_t t = 1000 * kMs;
  assert(!monitor.note_frame(t).hitch);  // no interval yet
  // Steady 33 ms frames with a little jitter: never a hitch.
  for (int i = 0; i < 40; ++i) {
    t += (i % 2 ? 32 : 34) * kMs;
    const auto verdict = monitor.note_frame(t);
    assert(!verdict.hitch);
    if (i >= 8) assert(verdict.expected_ms >= 32.0 && verdict.expected_ms <= 34.0);
  }
  // 40 ms is within 33 + max(8, 8.25); 45 ms is not.
  t += 40 * kMs;
  assert(!monitor.note_frame(t).hitch);
  t += 45 * kMs;
  const auto hitch = monitor.note_frame(t);
  assert(hitch.hitch && hitch.interval_ms == 45.0);
  // One hitch does not move the median.
  t += 33 * kMs;
  assert(!monitor.note_frame(t).hitch);
}

void test_no_verdict_without_history() {
  HitchMonitor monitor;
  monitor.note_frame(kMs);
  // A long frame right at the start has no median to compare with.
  assert(!monitor.note_frame(500 * kMs).hitch);
}

void test_activity_window_and_folding() {
  HitchMonitor::Options options;
  options.sample_capacity = 8;  // wraps: only the last 8 samples remain
  HitchMonitor monitor(options);
  for (std::uint64_t i = 0; i < 12; ++i) {
    // Thread 22 in function 0x823A7400 (two addresses), thread 1 blocked (0).
    monitor.add_sample({i * kMs, 22u, i % 2 ? 0x823A7484u : 0x823A7498u});
    monitor.add_sample({i * kMs, 1u, 0u});
  }
  // Samples 8..11 of each thread survive; window [9, 11) holds 9 and 10.
  const auto activity = monitor.activity_between(9 * kMs, 11 * kMs, 4, [](std::uint32_t pc) {
    return pc == 0u ? 0u : (pc & ~0xFFu);
  });
  assert(activity.size() == 2u);
  for (const auto& thread : activity) {
    assert(thread.samples == 2u);
    assert(thread.top.size() == 1u && thread.top[0].second == 2u);
    if (thread.thread == 22u) assert(thread.top[0].first == 0x823A7400u);
    if (thread.thread == 1u) assert(thread.top[0].first == 0u);
  }
  // Without a fold, addresses stay apart.
  const auto raw = monitor.activity_between(0, 100 * kMs, 4);
  for (const auto& thread : raw)
    if (thread.thread == 22u) assert(thread.top.size() == 2u && thread.samples == 4u);
}

void test_slow_event_tallies() {
  HitchMonitor::Options options;
  options.detailed_per_name = 2;
  HitchMonitor monitor(options);
  assert(monitor.note_slow("kernel", "NtReadFile", 6 * kMs));
  assert(monitor.note_slow("kernel", "NtReadFile", 9 * kMs));
  assert(!monitor.note_slow("kernel", "NtReadFile", 7 * kMs));
  assert(monitor.note_slow("callback", "audio render", 30 * kMs));
  const auto tallies = monitor.take_slow_tallies();
  assert(tallies.size() == 2u);
  assert(tallies[0].name == "audio render");  // the larger total first
  assert(tallies[1].count == 3u && tallies[1].total_ns == 22 * kMs &&
         tallies[1].max_ns == 9 * kMs);
  assert(monitor.take_slow_tallies().empty());
  // Detail stays limited after the tallies were taken.
  assert(!monitor.note_slow("kernel", "NtReadFile", 6 * kMs));
}

}  // namespace

int main() {
  test_frame_hitches();
  test_no_verdict_without_history();
  test_activity_window_and_folding();
  test_slow_event_tallies();
  return 0;
}
