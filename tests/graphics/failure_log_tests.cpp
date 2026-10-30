#include <cassert>
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

}  // namespace

int main() {
  test_counts_each_distinct_reason_in_first_seen_order();
  test_the_same_message_for_a_draw_and_a_resolve_stays_apart();
  test_bounded_past_the_limit();
  test_concurrent_record_and_read();
  std::cout << "gpu failure log tests passed" << std::endl;
  return 0;
}
