#include "xenon/logging/activity_trace.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "xenon/logging/probe_log.hpp"

namespace {

using namespace xenon::logging;

struct Capture {
  std::vector<std::string> lines;
  ActivityCondenser condenser{"T1 test",
                              [this](const std::string& line) { lines.push_back(line); }};
};

std::uint32_t kind(const char* name) {
  return intern_activity({"test", name, {"a", "", "", ""}, true});
}

ActivityEvent event(std::uint32_t key, std::uint64_t at_ms, std::uint64_t duration_us = 10,
                    std::uint64_t a = 0) {
  return ActivityEvent{key, {a, 0, 0, 0}, 0, at_ms * 1'000'000ull, duration_us * 1000ull, {}};
}

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

std::string joined(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& line : lines) out += line + '\n';
  return out;
}

// Interning is stable and distinguishes labels.
void test_interning() {
  const auto a = kind("alpha");
  assert(a == kind("alpha"));
  assert(a != kind("beta"));
  assert(a != intern_activity({"test", "alpha", {"b", "", "", ""}, true}));
  assert(activity_kind(a).name == "alpha");
  assert(activity_kind(0xFFFFFFu).name == "unknown");
}

// One event repeated condenses to a single entry with its count, average and
// maximum - including the first occurrences that started it.
void test_single_event_run() {
  Capture c;
  const auto wait = kind("wait");
  for (std::uint64_t i = 0; i < 100; ++i) c.condenser.add(event(wait, i, i == 50 ? 900 : 10, 7));
  c.condenser.flush();
  assert(c.lines.size() == 1);
  assert(contains(c.lines[0], "x100"));
  assert(contains(c.lines[0], "test wait(a=0x7)"));
  assert(contains(c.lines[0], "max 0.90 ms"));
}

// A repeating cycle of several events is one entry listing each step; the
// longest cycle wins over a repeat inside it ("A A B" x4 is a cycle of 3).
void test_cycle() {
  Capture c;
  const auto a = kind("a"), b = kind("b");
  std::uint64_t t = 0;
  for (int i = 0; i < 4; ++i) {
    c.condenser.add(event(a, t++));
    c.condenser.add(event(a, t++));
    c.condenser.add(event(b, t++));
  }
  c.condenser.flush();
  const auto text = joined(c.lines);
  assert(contains(c.lines[0], "x4 cycle of 3"));
  assert(c.lines.size() == 4);  // header + three steps
  assert(contains(c.lines[1], "test a("));
  assert(contains(c.lines[3], "test b("));
}

// Events that do not repeat are written one per line, in order, and a cycle
// interrupted midway reports the extra steps it saw.
void test_singles_and_interruption() {
  Capture c;
  const auto a = kind("a"), b = kind("b"), x = kind("x"), y = kind("y");
  c.condenser.add(event(x, 0));
  for (std::uint64_t i = 0; i < 6; ++i) c.condenser.add(event(i % 2 ? b : a, 1 + i));
  c.condenser.add(event(a, 7));  // half a cycle more
  c.condenser.add(event(y, 8));
  c.condenser.flush();
  const auto text = joined(c.lines);
  assert(contains(c.lines.front(), "test x("));
  assert(contains(text, "x3 (+1 steps) cycle of 2"));
  assert(contains(c.lines.back(), "test y("));
}

// A cycle that keeps going is written every second and marked as continuing,
// so the log stays in time order during a long stall.
void test_long_cycle_is_written_as_it_goes() {
  Capture c;
  const auto spin = kind("spin");
  for (std::uint64_t ms = 0; ms < 3500; ms += 5) c.condenser.add(event(spin, ms));
  assert(!c.lines.empty());
  assert(contains(c.lines.front(), "(continues)"));
  c.condenser.flush();
  assert(contains(c.lines.back(), "[continued]"));
  std::uint64_t total = 0;
  for (const auto& line : c.lines) {
    const auto at = line.find("  x");
    assert(at != std::string::npos);
    total += std::stoull(line.substr(at + 3));
  }
  assert(total == 700);  // every event is counted exactly once
}

// More than a window of distinct events is not held back indefinitely.
void test_window_is_bounded() {
  Capture c;
  for (std::uint64_t i = 0; i < 40; ++i)
    c.condenser.add(event(kind(("k" + std::to_string(i)).c_str()), i));
  assert(c.lines.size() >= 40 - ActivityCondenser::kWindow);
  c.condenser.flush();
  assert(c.lines.size() == 40);
}

// Events with text (a file path) only condense with the same text.
void test_text_keeps_events_apart() {
  Capture c;
  const auto open = kind("open");
  for (int i = 0; i < 3; ++i) {
    auto e = event(open, i);
    e.text = "game:/file" + std::to_string(i);
    c.condenser.add(e);
  }
  for (int i = 3; i < 6; ++i) {
    auto e = event(open, i);
    e.text = "game:/same";
    c.condenser.add(e);
  }
  c.condenser.flush();
  assert(c.lines.size() == 4);
  assert(contains(c.lines[0], "file0") && contains(c.lines[2], "file2"));
  assert(contains(c.lines[3], "x3") && contains(c.lines[3], "same"));
}

// End to end: per-thread streams, the trace file and the summary.
void test_trace_files() {
  const auto directory = std::filesystem::temp_directory_path() / "xenon_activity_trace_test";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  set_probe_log_directory(directory.string());
  set_activity_trace_enabled(true);
  const auto key = intern_activity({"xbox", "KeWaitForSingleObject", {"r3", "r4", "", ""}, true});
  std::thread worker([&] {
    set_activity_thread_label("T9 worker");
    for (int i = 0; i < 50; ++i) {
      ActivityScope scope(key, {0x1DC, 0, 0, 0});
      scope.result = 0x102;
    }
  });
  worker.join();
  record_activity(key, {1, 2, 0, 0}, 0, activity_now_ns(), 1000, "once");
  flush_activity_trace();
  std::ifstream trace(directory / "activity_trace.log");
  std::stringstream trace_text;
  trace_text << trace.rdbuf();
  assert(contains(trace_text.str(), "T9 worker  x50  xbox KeWaitForSingleObject(r3=0x1DC r4=0x0) -> 0x102"));
  assert(contains(trace_text.str(), "\"once\""));
  std::ifstream summary(directory / "activity_summary.txt");
  std::stringstream summary_text;
  summary_text << summary.rdbuf();
  assert(contains(summary_text.str(), "51x"));
  assert(contains(summary_text.str(), "== T9 worker"));
  set_activity_trace_enabled(false);
}

}  // namespace

int main() {
  test_interning();
  test_single_event_run();
  test_cycle();
  test_singles_and_interruption();
  test_long_cycle_is_written_as_it_goes();
  test_window_is_bounded();
  test_text_keeps_events_apart();
  test_trace_files();
  std::cout << "xenon_activity_trace_tests: ok\n";
  return 0;
}
