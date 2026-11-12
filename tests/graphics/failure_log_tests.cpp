#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "xenon/gpu/failure_log.hpp"

using xenon::gpu::GpuFailureLog;

namespace {

void test_counts_each_distinct_reason_in_first_seen_order() {
  GpuFailureLog log;
  assert(log.reasons().empty());
  assert(log.record("pixel shader 0x1234 did not compile", false) == 1u);
  assert(log.record("resolve destination is not mapped", true) == 1u);
  assert(log.record("pixel shader 0x1234 did not compile", false) == 2u);
  assert(log.record("pixel shader 0x1234 did not compile", false) == 3u);
  const auto reasons = log.reasons();
  assert(reasons.size() == 2u);
  assert(reasons[0].message == "pixel shader 0x1234 did not compile");
  assert(!reasons[0].resolve && reasons[0].count == 3u);
  assert(reasons[1].message == "resolve destination is not mapped");
  assert(reasons[1].resolve && reasons[1].count == 1u);
  assert(log.unlisted() == 0u);
}

void test_the_same_message_for_a_draw_and_a_resolve_stays_apart() {
  GpuFailureLog log;
  log.record("no bound guest memory", false);
  log.record("no bound guest memory", true);
  assert(log.reasons().size() == 2u);
}

void test_bounded_past_the_limit() {
  GpuFailureLog log;
  for (std::size_t i = 0; i < GpuFailureLog::kMaxReasons + 5u; ++i) {
    log.record("reason " + std::to_string(i), false);
  }
  assert(log.record("reason 0", false) == 2u);  // a kept reason still counts
  assert(log.record("one more new reason", false) == 0u);  // not kept
  const auto reasons = log.reasons();
  assert(reasons.size() == GpuFailureLog::kMaxReasons);
  assert(reasons[0].count == 2u);
  assert(log.unlisted() == 6u);
}

// The GPU thread records while another thread (the stop report) reads.
void test_concurrent_record_and_read() {
  GpuFailureLog log;
  std::thread writer([&] {
    for (int i = 0; i < 10000; ++i) log.record("draw failed", false);
  });
  std::size_t seen = 0;
  for (int i = 0; i < 1000; ++i) seen += log.reasons().size();
  writer.join();
  const auto reasons = log.reasons();
  assert(reasons.size() == 1u && reasons[0].count == 10000u);
  (void)seen;
}

// Each kind of resolve is counted with how many wrote only zeros, and only
// the first all-zero output of a kind is reported as new.
void test_resolve_census() {
  xenon::gpu::GpuResolveCensus census;
  assert(!census.record(false, 6, 26, 1280, 720, false, 0x1C150000u));
  assert(census.record(false, 6, 26, 1280, 720, true));    // first all-zero
  assert(!census.record(false, 6, 26, 1280, 720, true));   // already reported
  assert(!census.record(true, 0, 22, 1280, 720, false));
  assert(census.record(false, 6, 26, 640, 360, true));     // another size is another kind
  const auto kinds = census.kinds();
  assert(kinds.size() == 3u);
  assert(!kinds[0].depth && kinds[0].source_format == 6 && kinds[0].destination_format == 26);
  assert(kinds[0].width == 1280u && kinds[0].height == 720u);
  assert(kinds[0].count == 3u && kinds[0].all_zero == 2u);
  assert(kinds[0].last_destination == 0u);  // the latest resolve gave none
  assert(kinds[1].depth && kinds[1].count == 1u && kinds[1].all_zero == 0u);
  assert(kinds[2].count == 1u && kinds[2].all_zero == 1u);
  census.record(true, 0, 22, 1280, 720, false, 0x1B688000u);
  assert(census.kinds()[1].last_destination == 0x1B688000u);

  xenon::gpu::GpuResolveCensus full;
  for (std::uint32_t i = 0; i < xenon::gpu::GpuResolveCensus::kMaxKinds + 3u; ++i) {
    full.record(false, 6, 26, i + 1u, 1, false);
  }
  assert(full.kinds().size() == xenon::gpu::GpuResolveCensus::kMaxKinds);
  assert(full.unlisted() == 3u);
}

}  // namespace

int main() {
  test_resolve_census();
  test_counts_each_distinct_reason_in_first_seen_order();
  test_the_same_message_for_a_draw_and_a_resolve_stays_apart();
  test_bounded_past_the_limit();
  test_concurrent_record_and_read();
  std::cout << "gpu failure log tests passed" << std::endl;
  return 0;
}
