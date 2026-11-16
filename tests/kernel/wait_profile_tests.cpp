// Wait attribution: a blocking wait is recorded against the waiting thread,
// the object that ended it and that object's releasing signal (thread,
// caller, export), with its blocked time; a wait satisfied at once is not;
// a timeout counts as one that ended without the object; nothing is
// recorded while the profile is off.

#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "xenon/kernel/event.hpp"
#include "xenon/kernel/wait.hpp"
#include "xenon/kernel/wait_profile.hpp"

using namespace xenon::kernel;
using namespace std::chrono_literals;

namespace {

void await_waiter(KernelObject& object) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (waiter_count(object) == 0) {
    assert(std::chrono::steady_clock::now() < deadline && "waiter never registered");
    std::this_thread::yield();
  }
}

}  // namespace

int main() {
  reset_wait_profile();
  set_wait_profile_enabled(true);

  auto event = std::make_shared<KernelEvent>(false, false);
  // Thread 7 blocks; thread 9 releases it from guest code at 0x82001234.
  std::thread waiter([&] { assert(wait_for_single_object(event, -1ms, 7u) == WaitResult::Success); });
  await_waiter(*event);
  std::this_thread::sleep_for(30ms);
  event->note_signal(9u, 0x82001234u, "NtSetEvent");
  event->set();
  waiter.join();

  // Satisfied at once: not a blocking wait.
  event->set();
  assert(wait_for_single_object(event, 0ms, 7u) == WaitResult::Success);

  // A timeout: blocked, ended without the object.
  auto idle = std::make_shared<KernelEvent>(true, false);
  assert(wait_for_single_object(idle, 20ms, 7u) == WaitResult::Timeout);

  auto entries = wait_profile_entries();
  assert(entries.size() == 2u);
  const auto& released = entries[0].object_id == event->object_id() ? entries[0] : entries[1];
  const auto& timed_out = entries[0].object_id == idle->object_id() ? entries[0] : entries[1];
  assert(released.thread_id == 7u && released.waits == 1u && released.timeouts == 0u);
  assert(released.object_type == ObjectType::Event);
  assert(released.signaler_thread_id == 9u && released.signaler_caller == 0x82001234u);
  assert(std::string(released.signal_source) == "NtSetEvent");
  assert(released.total_ns >= 25'000'000u && released.max_ns == released.total_ns);
  assert(timed_out.waits == 1u && timed_out.timeouts == 1u && timed_out.signaler_thread_id == 0u);
  assert(timed_out.total_ns >= 15'000'000u);

  const auto report = wait_profile_report();
  assert(report.find("thread 7:") != std::string::npos);
  assert(report.find("T9 from 0x82001234 via NtSetEvent") != std::string::npos);
  assert(report.find("never signalled") != std::string::npos);

  // Off: nothing more is recorded.
  set_wait_profile_enabled(false);
  assert(wait_for_single_object(idle, 10ms, 7u) == WaitResult::Timeout);
  assert(wait_profile_entries().size() == 2u);
  assert(wait_profile_entries()[0].waits + wait_profile_entries()[1].waits == 2u);

  std::cout << "xenon_kernel_wait_profile_tests: ok\n";
  return 0;
}
