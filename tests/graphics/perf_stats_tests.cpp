#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "xenon/gpu/perf_stats.hpp"
#include "xenon/logging/probe_log.hpp"

namespace perf = xenon::gpu::perf;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

void test_line_reports_the_window_and_resets() {
  (void)perf::take_line(1.0);  // start from an empty window
  const auto start = Clock::now();
  // Four presents 33.3 ms apart: three intervals.
  for (int i = 0; i < 4; ++i) perf::note_presented_frame(start + i * 33333us);
  perf::add(perf::Counter::Swaps, 30u);
  perf::add(perf::Counter::FenceWaitNs, 12'000'000u);
  perf::add(perf::Counter::FenceWaits, 3u);
  perf::add(perf::Counter::ShaderCompileNs, 250'000'000u);
  perf::add(perf::Counter::ShaderCompiles, 2u);
  perf::add(perf::Counter::PipelineCreateNs, 40'000'000u);
  perf::add(perf::Counter::PipelineCreates, 4u);
  perf::add(perf::Counter::Draws, 2000u);
  perf::add(perf::Counter::Resolves, 20u);
  perf::add(perf::Counter::ReadbackBytes, 3u * 1024u * 1024u);
  perf::add(perf::Counter::ReadbackWaitNs, 7'000'000u);
  perf::add(perf::Counter::ReadbackWaits, 5u);
  perf::add(perf::Counter::ReadbackWindowNs, 9'000'000u);
  perf::add(perf::Counter::BackendNs, 610'000'000u);
  perf::add(perf::Counter::BackendWaitNs, 45'000'000u);
  perf::add(perf::Counter::BackendWaits, 1900u);
  perf::add(perf::Counter::Submits, 31u);
  perf::add(perf::Counter::SubmitNs, 4'000'000u);
  perf::add(perf::Counter::HoldsBarrier, 90u);
  perf::add(perf::Counter::HoldsSync, 7u);
  perf::add(perf::Counter::HoldsRead, 3u);
  perf::add(perf::Counter::HoldsDrain, 570u);
  perf::add(perf::Counter::BindingsReused, 1500u);
  perf::add(perf::Counter::BindingsWritten, 400u);
  perf::add(perf::Counter::ShaderWaitNs, 9'000'000u);
  perf::add(perf::Counter::ShaderWaits, 12u);
  perf::add(perf::Counter::GpuBusyNs, 21'000'000u);
  perf::add(perf::Counter::GpuIdleNs, 640'000'000u);
  perf::add(perf::Counter::GpuBatches, 540u);
  const auto line = perf::take_line(1.0);
  assert(contains(line, "frames 3.0/s"));
  assert(contains(line, "p50 33.3"));
  assert(contains(line, "max 33.3"));
  assert(contains(line, "swaps 30.0/s"));
  assert(contains(line, "fence 12 ms (3 waits)"));
  assert(contains(line, "shader 2 (250 ms)"));
  assert(contains(line, "pso 4 (40 ms)"));
  assert(contains(line, "draws 2000/s resolves 20/s"));
  assert(contains(line, "readback 3.0 MB wait 7 ms (5) window 9 ms"));
  assert(contains(line, "backend 610 ms wait 45 ms (1900)"));
  assert(contains(line, "submit 31 (4 ms)"));
  assert(contains(line, "holds barrier 90 sync 7 read 3 drain 570"));
  assert(contains(line, "bindings reused 1500 written 400"));
  assert(contains(line, "shader wait 9 ms (12)"));
  assert(contains(line, "gpu busy 21 ms idle 640 ms (540 batches)"));
  // Taken: the next window starts empty (the last present still anchors the
  // next interval).
  const auto empty = perf::take_line(1.0);
  assert(contains(empty, "frames 0.0/s"));
  assert(contains(empty, "swaps 0.0/s"));
  assert(contains(empty, "fence 0 ms (0 waits)"));
  assert(contains(empty, "draws 0/s"));
  assert(contains(empty, "backend 0 ms wait 0 ms (0)"));
  assert(contains(empty, "submit 0 (0 ms)"));
  for (const auto counter : {perf::Counter::Swaps, perf::Counter::Draws, perf::Counter::FenceWaits})
    assert(perf::peek(counter) == 0u);
}

void test_rates_are_per_second_of_the_window() {
  (void)perf::take_line(1.0);
  perf::add(perf::Counter::Draws, 1000u);
  perf::add(perf::Counter::Swaps, 15u);
  const auto line = perf::take_line(0.5);
  assert(contains(line, "draws 2000/s"));
  assert(contains(line, "swaps 30.0/s"));
}

void test_p99_and_max_show_a_hitch() {
  auto t = Clock::now() + 10s;
  perf::note_presented_frame(t);  // anchors this window's first interval
  (void)perf::take_line(1.0);     // (dropping the gap since the last test)
  for (int i = 0; i < 99; ++i) perf::note_presented_frame(t += 33333us);
  perf::note_presented_frame(t += 250ms);
  const auto line = perf::take_line(1.0);
  assert(contains(line, "p50 33.3"));
  assert(contains(line, "max 250.0"));
}

void test_scoped_time_counts_one_and_adds_its_duration() {
  (void)perf::take_line(1.0);
  {
    const perf::ScopedTime time(perf::Counter::PipelineCreateNs, perf::Counter::PipelineCreates);
    const auto until = Clock::now() + 2ms;
    while (Clock::now() < until) {
    }
  }
  assert(perf::peek(perf::Counter::PipelineCreates) == 1u);
  assert(perf::peek(perf::Counter::PipelineCreateNs) >= 2'000'000u);
}

void test_flush_writes_once_a_second_without_probe_logs() {
  const auto directory = std::filesystem::temp_directory_path() / "xenon_perf_stats_test";
  std::filesystem::remove_all(directory);
  xenon::logging::set_probe_logs_enabled(false);
  xenon::logging::set_probe_log_directory(directory.string());
  (void)perf::take_line(1.0);
  const auto start = Clock::now() + 100s;
  perf::flush_if_due(start);  // opens the first window
  perf::add(perf::Counter::Draws, 7u);
  perf::flush_if_due(start + 500ms);  // not a second yet
  perf::flush_if_due(start + 1s);     // writes
  perf::flush_if_due(start + 1500ms);  // not a second since the write
  xenon::logging::set_probe_log_directory("");  // releases the file
  std::string contents;
  {
    std::ifstream file(directory / "perf_frame.log");
    assert(file);
    std::stringstream text;
    text << file.rdbuf();
    contents = text.str();
  }
  std::size_t lines = 0;
  for (const char c : contents) lines += c == '\n';
  assert(lines == 1u);
  assert(contains(contents, "draws 7/s"));
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

}  // namespace

int main() {
  std::cerr << "line_reports_the_window_and_resets\n";
  test_line_reports_the_window_and_resets();
  std::cerr << "rates_are_per_second_of_the_window\n";
  test_rates_are_per_second_of_the_window();
  std::cerr << "p99_and_max_show_a_hitch\n";
  test_p99_and_max_show_a_hitch();
  std::cerr << "scoped_time_counts_one_and_adds_its_duration\n";
  test_scoped_time_counts_one_and_adds_its_duration();
  std::cerr << "flush_writes_once_a_second_without_probe_logs\n";
  test_flush_writes_once_a_second_without_probe_logs();
  std::cout << "perf stats tests passed\n";
  return 0;
}
