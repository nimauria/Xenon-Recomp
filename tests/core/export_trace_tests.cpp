#include <array>
#include <cassert>
#include <cstdint>
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

}  // namespace

int main() {
  test_disabled_and_capture();
  test_bounded_order_and_thread_filter();
  test_multithread_safety();
  return 0;
}
