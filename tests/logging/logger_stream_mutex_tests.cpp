// Regression coverage for a real crash: multiple guest threads writing to
// std::cout/std::cerr through two independent mechanisms (XenonSession's own
// diagnostics and xenon::logging::Logger's default no-sink path) raced the
// CRT's lazy buffering-mode detection for a freopen'd stdout, fast-failing
// the whole process (observed via a debugger stack trace: MSVCP140
// basic_filebuf::xsputn -> ucrtbase!fwrite -> isatty_proc ->
// invalid_parameter_noinfo). The fix routes both through one shared mutex,
// xenon::logging::Logger::stream_mutex() - this test locks in that they stay
// unified, and that concurrent Logger::log() calls never crash or corrupt
// stdout's internal state.

#include "xenon/logging/logger.hpp"

#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

using namespace xenon::logging;

namespace {

void test_stream_mutex_is_a_single_shared_instance() {
  // Logger is a singleton, so stream_mutex() must return the exact same
  // mutex object on every call - two independent mutexes would each
  // serialize their own call sites but do nothing to stop them racing
  // each other.
  assert(&Logger::stream_mutex() == &Logger::stream_mutex());
}

void test_concurrent_default_sink_logging_does_not_crash() {
  // No sink installed: log() takes the default (no-sink) path, which writes
  // directly to std::cout/std::cerr under stream_mutex(). Hammer it from
  // many threads simultaneously - this reproduces the exact concurrency
  // shape (many guest threads logging at once) that used to crash the
  // process; success here is simply completing without an unhandled
  // exception or a debugger-visible fault.
  Logger::instance().set_sink(nullptr);
  constexpr int kThreads = 8;
  constexpr int kIterationsPerThread = 200;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t] {
      for (int i = 0; i < kIterationsPerThread; ++i) {
        Logger::instance().log(Level::Warning, "test", "concurrent stream write");
      }
    });
  }
  for (auto& thread : threads) thread.join();
}

}  // namespace

int main() {
  std::cout << "Testing Logger stream_mutex()...\n";

  test_stream_mutex_is_a_single_shared_instance();
  test_concurrent_default_sink_logging_does_not_crash();

  std::cout << "All logger stream_mutex tests passed!\n";
  return 0;
}
