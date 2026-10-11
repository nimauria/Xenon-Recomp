// User-mode APCs (KernelThread::queue_user_apc()) and alertable waits.
//
// NT semantics, which xenia and rexglue-sdk follow: an APC queued for a
// thread runs only when that thread makes an alertable wait or delay. Such a
// wait returns STATUS_USER_APC (WaitResult::UserApc) when an APC is pending
// and no waited object is satisfiable; a satisfiable object is taken first.
// A non-alertable wait is not ended by an APC. The export layer runs the
// APCs after the wait returns.

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "xenon/kernel/event.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/kernel/wait.hpp"

using namespace xenon::kernel;
using namespace std::chrono_literals;

namespace {

constexpr auto kInfinite = std::chrono::milliseconds(-1);

// Runs `body` on a KernelThread and returns once it has finished.
void on_kernel_thread(std::function<void(KernelThread&)> body) {
  ThreadCreationParams params{};
  std::shared_ptr<KernelThread> thread;
  thread = std::make_shared<KernelThread>(
      [&]() -> std::uint32_t {
        body(*KernelThread::current());
        return 0;
      },
      params);
  assert(thread->start());
  assert(thread->join(10000));
}

void test_an_apc_queued_during_an_alertable_wait_ends_it() {
  auto never = std::make_shared<KernelEvent>(true, false);
  std::atomic<KernelThread*> target{nullptr};
  std::thread queuer([&] {
    while (!target.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(30ms);
    target.load()->queue_user_apc({0x82001000u, 1u, 2u, 3u});
  });
  on_kernel_thread([&](KernelThread& self) {
    target = &self;
    const auto started = std::chrono::steady_clock::now();
    assert(wait_for_single_object(never, kInfinite, 1, /*alertable=*/true) ==
           WaitResult::UserApc);
    assert(std::chrono::steady_clock::now() - started < 2000ms);
    const auto apcs = self.take_user_apcs();
    assert(apcs.size() == 1 && apcs[0].routine == 0x82001000u && apcs[0].context == 1u &&
           apcs[0].arg1 == 2u && apcs[0].arg2 == 3u);
    assert(!self.user_apc_pending());
  });
  queuer.join();
}

void test_a_pending_apc_ends_an_alertable_wait_at_once() {
  auto never = std::make_shared<KernelEvent>(true, false);
  on_kernel_thread([&](KernelThread& self) {
    self.queue_user_apc({0x82001000u, 0u, 0u, 0u});
    assert(wait_for_single_object(never, 5000ms, 1, true) == WaitResult::UserApc);
    assert(delay_current_thread(5000ms, true) == WaitResult::UserApc);
    std::vector<std::shared_ptr<KernelObject>> objects{never};
    std::uint32_t index = 99;
    assert(wait_for_multiple_objects(objects, false, 5000ms, &index, 1, true) ==
           WaitResult::UserApc);
    assert(self.take_user_apcs().size() == 1);
  });
}

void test_a_non_alertable_wait_ignores_apcs() {
  auto never = std::make_shared<KernelEvent>(true, false);
  on_kernel_thread([&](KernelThread& self) {
    self.queue_user_apc({0x82001000u, 0u, 0u, 0u});
    assert(wait_for_single_object(never, 30ms, 1, /*alertable=*/false) == WaitResult::Timeout);
    assert(delay_current_thread(10ms, false) == WaitResult::Success);
    assert(self.user_apc_pending() && "the APC is still queued for a later alertable wait");
  });
}

void test_a_satisfiable_object_wins_over_a_pending_apc() {
  auto event = std::make_shared<KernelEvent>(false, true);  // auto-reset, signalled
  on_kernel_thread([&](KernelThread& self) {
    self.queue_user_apc({0x82001000u, 0u, 0u, 0u});
    assert(wait_for_single_object(event, 5000ms, 1, true) == WaitResult::Success);
    assert(!event->signaled() && "the wait took the event");
    assert(self.user_apc_pending());
  });
}

void test_an_apc_ends_an_alertable_delay() {
  std::atomic<KernelThread*> target{nullptr};
  std::thread queuer([&] {
    while (!target.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(30ms);
    target.load()->queue_user_apc({0x82001000u, 0u, 0u, 0u});
  });
  on_kernel_thread([&](KernelThread& self) {
    target = &self;
    const auto started = std::chrono::steady_clock::now();
    assert(delay_current_thread(std::chrono::minutes(10), true) == WaitResult::UserApc);
    assert(std::chrono::steady_clock::now() - started < 2000ms);
    static_cast<void>(self.take_user_apcs());
  });
  queuer.join();
}

void test_apcs_are_taken_in_queue_order() {
  on_kernel_thread([&](KernelThread& self) {
    for (std::uint32_t i = 0; i < 5; ++i) self.queue_user_apc({0x82001000u + i, i, 0u, 0u});
    const auto apcs = self.take_user_apcs();
    assert(apcs.size() == 5);
    for (std::uint32_t i = 0; i < 5; ++i) assert(apcs[i].context == i);
    assert(self.take_user_apcs().empty());
  });
}

}  // namespace

int main() {
  std::cout << "Testing user APCs and alertable waits...\n";
  test_an_apc_queued_during_an_alertable_wait_ends_it();
  test_a_pending_apc_ends_an_alertable_wait_at_once();
  test_a_non_alertable_wait_ignores_apcs();
  test_a_satisfiable_object_wins_over_a_pending_apc();
  test_an_apc_ends_an_alertable_delay();
  test_apcs_are_taken_in_queue_order();
  std::cout << "All user APC tests passed!\n";
  return 0;
}
