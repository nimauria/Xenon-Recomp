#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>

#include "xenon/core/in_flight_calls.hpp"

using xenon::core::InFlightCalls;

namespace {

InFlightCalls::Call make_call(std::uint32_t thread_id, std::uint32_t ordinal) {
  InFlightCalls::Call call{};
  call.thread_id = thread_id;
  call.ordinal = ordinal;
  call.lr = 0x82000000u + ordinal;
  call.arguments = {ordinal, ordinal + 1u, ordinal + 2u, ordinal + 3u};
  call.since = std::chrono::steady_clock::now();
  return call;
}

void test_enter_leave_and_nesting() {
  InFlightCalls calls;
  assert(calls.snapshot().empty());
  {
    const InFlightCalls::Scope outer(calls, make_call(5, 10));
    auto now = calls.snapshot();
    assert(now.size() == 1u && now.at(5).ordinal == 10u && now.at(5).arguments[3] == 13u);
    {
      // A kernel call that runs guest code making another kernel call.
      const InFlightCalls::Scope inner(calls, make_call(5, 20));
      now = calls.snapshot();
      assert(now.size() == 1u && now.at(5).ordinal == 20u);
    }
    now = calls.snapshot();
    assert(now.size() == 1u && now.at(5).ordinal == 10u);
  }
  assert(calls.snapshot().empty());
}

void test_unwinding_leaves_the_call() {
  InFlightCalls calls;
  try {
    const InFlightCalls::Scope scope(calls, make_call(3, 1));
    throw std::runtime_error("thread terminated inside the call");
  } catch (const std::runtime_error&) {
  }
  assert(calls.snapshot().empty());
}

void test_blocked_threads_are_visible_to_another_thread() {
  InFlightCalls calls;
  constexpr std::uint32_t kThreads = 6;
  std::latch entered(kThreads);
  std::atomic<bool> release{false};
  std::vector<std::thread> workers;
  for (std::uint32_t thread = 1; thread <= kThreads; ++thread) {
    workers.emplace_back([&, thread] {
      const InFlightCalls::Scope scope(calls, make_call(thread, 100u + thread));
      entered.count_down();
      while (!release.load()) std::this_thread::yield();
    });
  }
  entered.wait();
  const auto blocked = calls.snapshot();
  assert(blocked.size() == kThreads);
  for (std::uint32_t thread = 1; thread <= kThreads; ++thread) {
    assert(blocked.at(thread).ordinal == 100u + thread);
    assert(blocked.at(thread).lr == 0x82000000u + 100u + thread);
  }
  release.store(true);
  for (auto& worker : workers) worker.join();
  assert(calls.snapshot().empty());
}

void test_reader_never_sees_a_torn_call() {
  InFlightCalls calls;
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  for (std::uint32_t thread = 1; thread <= 4; ++thread) {
    workers.emplace_back([&, thread] {
      for (std::uint32_t i = 0; !stop.load(std::memory_order_relaxed); ++i) {
        const InFlightCalls::Scope scope(calls, make_call(thread, i));
      }
    });
  }
  for (int pass = 0; pass < 20000; ++pass) {
    for (const auto& [thread, call] : calls.snapshot()) {
      assert(thread == call.thread_id && thread >= 1u && thread <= 4u);
      assert(call.lr == 0x82000000u + call.ordinal);
      assert(call.arguments[0] == call.ordinal && call.arguments[3] == call.ordinal + 3u);
    }
  }
  stop.store(true);
  for (auto& worker : workers) worker.join();
  assert(calls.snapshot().empty());
}

}  // namespace

int main() {
  test_enter_leave_and_nesting();
  test_unwinding_leaves_the_call();
  test_blocked_threads_are_visible_to_another_thread();
  test_reader_never_sees_a_torn_call();
  return 0;
}
