#include <array>
#include <cassert>
#include <cstdint>
#include <atomic>
#include <latch>
#include <thread>
#include <vector>

#include "xenon/core/export_trace.hpp"

using xenon::core::ExportTrace;

namespace {

void add(ExportTrace& trace, std::uint32_t thread_id, std::uint32_t ordinal,
         std::uint64_t result = 0) {
  const std::array<std::uint64_t, 8> args{
      ordinal, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
  trace.record(thread_id, "xboxkrnl.exe", "MmTest", ordinal,
               0x82000000u + ordinal, 0x82100000u, 0x82200000u, args,
               result, true, true, true);
}

void test_disabled_and_capture() {
  ExportTrace trace(4);
  add(trace, 7, 1);
  assert(trace.recent_global(4).empty());

  trace.set_enabled(true);
  add(trace, 7, 2, 0x1234u);
  const auto records = trace.recent_global(4);
  assert(records.size() == 1u);
  assert(records[0].sequence == 1u);
  assert(records[0].thread_id == 7u);
  assert(records[0].library_view() == "xboxkrnl.exe");
  assert(records[0].name_view() == "MmTest");
  assert(records[0].arguments[0] == 2u);
  assert(records[0].result_r3 == 0x1234u);
  assert(records[0].handler_found && records[0].handled && records[0].success);
}

void test_bounded_order_and_thread_filter() {
  ExportTrace trace(3);
  trace.set_enabled(true);
  add(trace, 1, 1);
  add(trace, 2, 2);
  add(trace, 1, 3);
  add(trace, 2, 4);

  const auto global = trace.recent_global(10);
  assert(global.size() == 3u);
  assert(global[0].ordinal == 2u);
  assert(global[1].ordinal == 3u);
  assert(global[2].ordinal == 4u);
  assert(global[0].sequence < global[1].sequence &&
         global[1].sequence < global[2].sequence);

  const auto thread = trace.recent_for_thread(2, 2);
  assert(thread.size() == 2u);
  assert(thread[0].ordinal == 2u && thread[1].ordinal == 4u);
}

void test_multithread_safety() {
  ExportTrace trace(512);
  trace.set_enabled(true);
  std::vector<std::thread> workers;
  for (std::uint32_t thread = 1; thread <= 4; ++thread) {
    workers.emplace_back([&trace, thread] {
      for (std::uint32_t i = 0; i < 250; ++i) add(trace, thread, i);
    });
  }
  for (auto& worker : workers) worker.join();

  const auto records = trace.recent_global(1024);
  assert(records.size() == 512u);
  for (std::size_t i = 1; i < records.size(); ++i) {
    assert(records[i - 1].sequence < records[i].sequence);
  }
  for (const auto& record : records) {
    assert(record.thread_id >= 1u && record.thread_id <= 4u);
  }
}

void test_call_timing_is_kept() {
  ExportTrace trace(4);
  trace.set_enabled(true);
  const std::array<std::uint64_t, 8> args{};
  trace.record(3, "xboxkrnl.exe", "NtReadFile", 1, 0x82000000u, 0, 0, args, 0, true, true,
               true, 5'000'000u, 7'250'000u);
  const auto records = trace.recent_global(4);
  assert(records.size() == 1u);
  assert(records[0].start_ns == 5'000'000u && records[0].duration_ns == 7'250'000u);
}

void test_order_across_threads_and_clear() {
  ExportTrace trace(8);
  trace.set_enabled(true);
  // Each call comes from its own host thread, one after another: the merged
  // trace keeps the order they happened in.
  for (std::uint32_t ordinal = 1; ordinal <= 6; ++ordinal) {
    std::thread([&trace, ordinal] { add(trace, ordinal % 2u, ordinal); }).join();
  }
  auto records = trace.recent_global(10);
  assert(records.size() == 6u);
  for (std::size_t i = 0; i < records.size(); ++i) {
    assert(records[i].ordinal == i + 1u);
    assert(records[i].sequence == i + 1u);
  }
  const auto odd = trace.recent_for_thread(1, 2);
  assert(odd.size() == 2u && odd[0].ordinal == 3u && odd[1].ordinal == 5u);

  trace.clear();
  assert(trace.recent_global(10).empty());
  add(trace, 9, 42);
  records = trace.recent_global(10);
  assert(records.size() == 1u && records[0].ordinal == 42u &&
         records[0].sequence == 1u);
}

void test_reader_never_sees_a_torn_record() {
  // Small lanes so writers lap their rings while the reader copies them.
  ExportTrace trace(4);
  trace.set_enabled(true);
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  for (std::uint32_t thread = 1; thread <= 4; ++thread) {
    workers.emplace_back([&trace, &stop, thread] {
      for (std::uint32_t i = 0; !stop.load(std::memory_order_relaxed); ++i) {
        add(trace, thread, i, std::uint64_t{thread} << 32 | i);
      }
    });
  }
  for (int pass = 0; pass < 2000; ++pass) {
    const auto records = trace.recent_global(16);
    assert(records.size() <= 4u);
    for (std::size_t i = 0; i < records.size(); ++i) {
      const auto& record = records[i];
      assert(record.sequence == i + 1u);
      assert(record.arguments[0] == record.ordinal);
      assert(record.call_address == 0x82000000u + record.ordinal);
      assert(record.result_r3 ==
             (std::uint64_t{record.thread_id} << 32 | record.ordinal));
      assert(record.name_view() == "MmTest");
    }
  }
  stop.store(true);
  for (auto& worker : workers) worker.join();
}

void test_threads_past_the_lane_limit_share_a_lane() {
  constexpr std::size_t kThreads = ExportTrace::kMaxLanes + 8u;
  ExportTrace trace(kThreads);
  trace.set_enabled(true);
  // Every thread is alive at once, so each one is a distinct writer.
  std::latch all_started(static_cast<std::ptrdiff_t>(kThreads));
  std::vector<std::thread> workers;
  for (std::uint32_t thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&trace, &all_started, thread] {
      all_started.arrive_and_wait();
      add(trace, thread, thread);
    });
  }
  for (auto& worker : workers) worker.join();

  const auto records = trace.recent_global(kThreads * 2u);
  assert(records.size() == kThreads);
  std::vector<bool> seen(kThreads, false);
  for (const auto& record : records) {
    assert(record.ordinal < kThreads && !seen[record.ordinal]);
    seen[record.ordinal] = true;
  }
}

}  // namespace

int main() {
  test_disabled_and_capture();
  test_bounded_order_and_thread_filter();
  test_multithread_safety();
  test_call_timing_is_kept();
  test_order_across_threads_and_clear();
  test_reader_never_sees_a_torn_record();
  test_threads_past_the_lane_limit_share_a_lane();
  return 0;
}
