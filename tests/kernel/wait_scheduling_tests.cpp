// Wait-aware scheduling: a zero or tiny KeDelayExecutionThread interval gives
// the processor up (as on the Xbox 360) instead of returning at once; wait
// timeouts and delays keep sub-millisecond precision and never end early; a
// registered wait that spins before it blocks is still granted by the signal,
// consumes exactly what a blocked waiter would, and blocks when the signal is
// slow; XENON_WAIT_SPIN's limit turns spinning off; and many waiters and
// signallers on auto-reset events and semaphores lose no wakeup and take no
// signal twice, with spinning on and off.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "xenon/kernel/event.hpp"
#include "xenon/kernel/mutant.hpp"
#include "xenon/kernel/semaphore.hpp"
#include "xenon/kernel/time.hpp"
#include "xenon/kernel/wait.hpp"
#include "xenon/kernel/wait_profile.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

using namespace xenon::kernel;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto kForever = std::chrono::milliseconds(-1);

void await_waiters(KernelObject& object, std::size_t count) {
  const auto deadline = Clock::now() + 10s;
  while (waiter_count(object) < count) {
    assert(Clock::now() < deadline && "waiter never registered");
    std::this_thread::yield();
  }
}

// Fails the test, rather than hanging it, if a stress phase loses a wakeup;
// `dump` prints the phase's state first (what is left on each object and
// who still waits), which tells a lost wakeup from a stranded signal.
class Watchdog {
 public:
  Watchdog(const char* phase, std::chrono::seconds limit, std::function<void()> dump)
      : thread_([this, phase, limit, dump = std::move(dump)] {
          const auto deadline = Clock::now() + limit;
          while (!done_.load(std::memory_order_acquire)) {
            if (Clock::now() > deadline) {
              std::fprintf(stderr, "%s: no progress within %lld s - a lost wakeup\n", phase,
                           static_cast<long long>(limit.count()));
              dump();
              std::fflush(stderr);
              std::abort();
            }
            std::this_thread::sleep_for(10ms);
          }
        }) {}
  ~Watchdog() {
    done_.store(true, std::memory_order_release);
    thread_.join();
  }

 private:
  std::atomic<bool> done_{false};
  std::thread thread_;
};

// Pins the calling thread to one processor; returns false where the host
// does not allow it.
bool pin_to_processor(unsigned processor) {
#if defined(_WIN32)
  return SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << processor) != 0;
#elif defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(processor, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)processor;
  return false;
#endif
}

unsigned last_allowed_processor() {
#if defined(_WIN32)
  DWORD_PTR process_mask = 0, system_mask = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) ||
      process_mask == 0)
    return 0;
  unsigned last = 0;
  for (unsigned i = 0; i < sizeof(DWORD_PTR) * 8; ++i)
    if (process_mask & (DWORD_PTR{1} << i)) last = i;
  return last;
#else
  const auto count = std::thread::hardware_concurrency();
  return count ? count - 1 : 0;
#endif
}

// With a partner thread ready on the same processor, a call that yields lets
// it run; one that returns at once does not (the partner only runs when this
// thread's quantum ends). Returns how many of `calls` let the partner run.
template <typename Call>
int partner_progress_across(Call call, int calls) {
  const unsigned processor = last_allowed_processor();
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> partner_steps{0};
  std::atomic<bool> partner_pinned{false};
  std::thread partner([&] {
    partner_pinned.store(pin_to_processor(processor));
    while (!stop.load(std::memory_order_relaxed)) {
      partner_steps.fetch_add(1, std::memory_order_relaxed);
      yield_current_thread();
    }
  });
  const bool pinned = pin_to_processor(processor);
  while (partner_steps.load() == 0) std::this_thread::sleep_for(1ms);
  assert(pinned && partner_pinned.load() && "the test needs to pin both threads");
  int progressed = 0;
  for (int i = 0; i < calls; ++i) {
    const auto before = partner_steps.load(std::memory_order_relaxed);
    call();
    if (partner_steps.load(std::memory_order_relaxed) != before) ++progressed;
  }
  stop.store(true);
  partner.join();
#if defined(_WIN32)
  SetThreadAffinityMask(GetCurrentThread(), ~DWORD_PTR{0} >> 1);
#endif
  return progressed;
}

void test_zero_delay_yields() {
  constexpr int kCalls = 200;
  const int zero = partner_progress_across(
      [] { assert(delay_current_thread(0ns) == WaitResult::Success); }, kCalls);
  std::cout << "zero delay: partner ran across " << zero << "/" << kCalls << " delays\n";
  assert(zero >= kCalls * 9 / 10 && "a zero delay must yield the processor");

  // A tiny delay (one 100 ns guest tick), shorter than the host can block for.
  const int tiny = partner_progress_across(
      [] { assert(delay_current_thread(100ns) == WaitResult::Success); }, kCalls);
  std::cout << "100 ns delay: partner ran across " << tiny << "/" << kCalls << " delays\n";
  assert(tiny >= kCalls * 9 / 10 && "a tiny delay must yield the processor");

  // NtYieldExecution's host yield.
  const int yields = partner_progress_across([] { yield_current_thread(); }, kCalls);
  assert(yields >= kCalls * 9 / 10);
}

std::chrono::microseconds elapsed_us(Clock::time_point since) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - since);
}

std::chrono::microseconds median(std::vector<std::chrono::microseconds> samples) {
  std::nth_element(samples.begin(), samples.begin() + samples.size() / 2, samples.end());
  return samples[samples.size() / 2];
}

// A sub-millisecond timeout or delay lasts at least as long as asked (the
// old millisecond conversion ended a 0.5 ms wait at once and cut 1.5 ms to
// 1 ms), and, on the high-resolution timer, not whole milliseconds longer.
void test_timeout_precision() {
  const HostTimerResolution resolution{};
  auto idle = std::make_shared<KernelEvent>(true, false);
  for (const auto timeout : {std::chrono::nanoseconds(300us), std::chrono::nanoseconds(1500us),
                             std::chrono::nanoseconds(2500us)}) {
    std::vector<std::chrono::microseconds> samples;
    for (int i = 0; i < 25; ++i) {
      const auto start = Clock::now();
      assert(wait_for_single_object(idle, timeout, 1u) == WaitResult::Timeout);
      const auto took = Clock::now() - start;
      assert(took >= timeout && "a timeout must never end early");
      samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(took));
    }
    const auto typical = median(samples);
    std::cout << "timeout " << std::chrono::duration_cast<std::chrono::microseconds>(timeout).count()
              << " us: median " << typical.count() << " us\n";
    assert(typical < timeout + 2ms);
  }

  for (const auto delay : {std::chrono::nanoseconds(30us), std::chrono::nanoseconds(150us),
                           std::chrono::nanoseconds(400us),
                           std::chrono::nanoseconds(1500us)}) {
    std::vector<std::chrono::microseconds> samples;
    for (int i = 0; i < 25; ++i) {
      const auto start = Clock::now();
      assert(delay_current_thread(delay) == WaitResult::Success);
      const auto took = Clock::now() - start;
      assert(took >= delay && "a delay must never end early");
      samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(took));
    }
    const auto typical = median(samples);
    std::cout << "delay " << std::chrono::duration_cast<std::chrono::microseconds>(delay).count()
              << " us: median " << typical.count() << " us\n";
    assert(typical < delay + 2ms);
  }

  // A signal still ends a sub-millisecond-timeout wait it arrives within.
  auto event = std::make_shared<KernelEvent>(false, false);
  std::thread setter([&] {
    await_waiters(*event, 1);
    event->set();
  });
  assert(wait_for_single_object(event, 10s, 1u) == WaitResult::Success);
  setter.join();
}

// The waiter spins, the signal comes while it does: the signaller grants the
// wait (an auto-reset event is consumed for it), the waiter never blocks.
// Then a signal that comes long after the spin: the waiter blocked, and the
// signal still wakes it. Then with spinning off: no wait spins.
void test_spin_then_block() {
  set_wait_spin_limit(100us);
  reset_wait_profile();
  auto event = std::make_shared<KernelEvent>(false, false);
  int hits_seen = 0;
  for (int i = 0; i < 200 && hits_seen == 0; ++i) {
    std::thread setter([&] {
      await_waiters(*event, 1);
      event->set();
    });
    assert(wait_for_single_object(event, kForever, 1u) == WaitResult::Success);
    setter.join();
    assert(!event->signaled() && "the granted wait consumed the auto-reset event");
    hits_seen = static_cast<int>(wait_spin_counters().spin_hits);
  }
  auto counters = wait_spin_counters();
  std::cout << "quick signal: " << counters.registered << " registered, " << counters.spins
            << " spun, " << counters.spin_hits << " spin hits, " << counters.blocks
            << " blocked\n";
  assert(counters.spins >= 1u && counters.spin_hits >= 1u && "a quick signal ends the spin");

  // Slow signal: spins, misses, blocks, and the signal still wakes it.
  reset_wait_profile();
  std::thread late_setter([&] {
    await_waiters(*event, 1);
    std::this_thread::sleep_for(30ms);
    event->set();
  });
  const auto start = Clock::now();
  assert(wait_for_single_object(event, kForever, 1u) == WaitResult::Success);
  assert(Clock::now() - start >= 25ms);
  late_setter.join();
  counters = wait_spin_counters();
  assert(counters.registered == 1u && counters.blocks == 1u && counters.spin_hits == 0u);
  assert(!event->signaled());

  // A semaphore taken while spinning takes exactly one unit; a mutant taken
  // while spinning is owned by the waiter.
  auto semaphore = std::make_shared<KernelSemaphore>(0, 10);
  auto mutant = std::make_shared<KernelMutant>(true, 2u);
  std::thread releaser([&] {
    await_waiters(*semaphore, 1);
    assert(semaphore->release(3));
    await_waiters(*mutant, 1);
    assert(mutant->release(2u));
  });
  assert(wait_for_single_object(semaphore, kForever, 1u) == WaitResult::Success);
  assert(wait_for_single_object(mutant, kForever, 1u) == WaitResult::Success);
  releaser.join();
  assert(semaphore->count() == 2);
  assert(mutant->owner_thread_id() == 1u && mutant->recursion_count() == 1);
  assert(mutant->release(1u));

  // Off (XENON_WAIT_SPIN=0): nothing spins, and waits still wake.
  set_wait_spin_limit(0ns);
  assert(wait_spin_limit() == 0ns);
  reset_wait_profile();
  for (int i = 0; i < 20; ++i) {
    std::thread setter([&] {
      await_waiters(*event, 1);
      event->set();
    });
    assert(wait_for_single_object(event, kForever, 1u) == WaitResult::Success);
    setter.join();
  }
  counters = wait_spin_counters();
  assert(counters.registered >= 1u && counters.spins == 0u && counters.spin_hits == 0u);
  const auto report = wait_profile_report();
  assert(report.find("spin: ") != std::string::npos);
  set_wait_spin_limit(4us);
}

// Many waiters and signallers on one auto-reset event: every waiter gets
// each of its wakes (none is lost: a lost one hangs, and the watchdog fails
// the test), and no set releases two waiters (wakes plus a set left pending
// never exceed the sets made). Some waits use short timeouts and retry, so
// timeouts race grants.
void stress_auto_reset_event(const char* phase) {
  constexpr int kWaiters = 8;
  constexpr int kSignallers = 4;
  constexpr int kWakesEach = 6000;
  auto event = std::make_shared<KernelEvent>(false, false);
  std::atomic<std::uint64_t> wakes{0};
  std::atomic<std::uint64_t> sets{0};
  std::atomic<int> finished{0};
  std::vector<std::atomic<int>> per_waiter(kWaiters);
  Watchdog watchdog(phase, std::chrono::seconds(120), [&] {
    std::fprintf(stderr, "  wakes %llu, sets %llu, signalled %d, registered waiters %zu\n",
                 static_cast<unsigned long long>(wakes.load()),
                 static_cast<unsigned long long>(sets.load()), event->signaled() ? 1 : 0,
                 waiter_count(*event));
    for (int w = 0; w < kWaiters; ++w)
      std::fprintf(stderr, "  waiter %d: %d of %d\n", w, per_waiter[w].load(), kWakesEach);
  });
  std::vector<std::thread> threads;
  for (int w = 0; w < kWaiters; ++w) {
    threads.emplace_back([&, w] {
      const bool timed = (w % 2) == 1;
      for (int i = 0; i < kWakesEach;) {
        const auto result = wait_for_single_object(
            event, timed ? std::chrono::nanoseconds(50us) : std::chrono::nanoseconds(kForever),
            static_cast<std::uint32_t>(w + 1));
        if (result == WaitResult::Timeout) {
          assert(timed);
          continue;
        }
        assert(result == WaitResult::Success);
        ++per_waiter[w];
        wakes.fetch_add(1, std::memory_order_relaxed);
        ++i;
      }
      finished.fetch_add(1);
    });
  }
  for (int s = 0; s < kSignallers; ++s) {
    threads.emplace_back([&] {
      while (finished.load() < kWaiters) {
        sets.fetch_add(1, std::memory_order_relaxed);
        event->set();
        if ((sets.load(std::memory_order_relaxed) & 7u) == 0) yield_current_thread();
      }
    });
  }
  for (auto& thread : threads) thread.join();
  const std::uint64_t pending = event->signaled() ? 1u : 0u;
  for (int w = 0; w < kWaiters; ++w) assert(per_waiter[w] == kWakesEach);
  assert(wakes.load() == static_cast<std::uint64_t>(kWaiters) * kWakesEach);
  assert(wakes.load() + pending <= sets.load() && "a set released two waiters");
  assert(waiter_count(*event) == 0u);
  std::cout << phase << ": " << wakes.load() << " wakes from " << sets.load() << " sets\n";
}

// Many waiters and signallers on semaphores: exactly as many waits succeed
// as units are released (none is lost to a timed-out wait, none is taken
// twice) and no count is left over. Half the waiters WaitAny over both
// semaphores, the rest wait on the first; half use short timeouts and retry.
// Every waiter takes units until the real ones run out, then exits on one
// stop unit released on the first semaphore, so no unit can be stranded
// where no remaining waiter can take it. A lost wakeup hangs a waiter, and
// the watchdog fails the test.
void stress_semaphores(const char* phase) {
  constexpr int kWaiters = 8;
  constexpr int kSignallers = 4;
  constexpr int kUnits = 64000;
  auto first = std::make_shared<KernelSemaphore>(0, kUnits + kWaiters);
  auto second = std::make_shared<KernelSemaphore>(0, kUnits + kWaiters);
  const std::shared_ptr<KernelObject> both[] = {first, second};
  std::atomic<int> taken{0};
  std::atomic<int> released{0};
  std::vector<std::atomic<int>> per_waiter(kWaiters);
  Watchdog watchdog(phase, std::chrono::seconds(120), [&] {
    std::fprintf(stderr,
                 "  taken %d, released %d, counts %d/%d, registered waiters %zu/%zu\n",
                 taken.load(), released.load(), first->count(), second->count(),
                 waiter_count(*first), waiter_count(*second));
    for (int w = 0; w < kWaiters; ++w)
      std::fprintf(stderr, "  waiter %d: %d taken\n", w, per_waiter[w].load());
  });
  std::vector<std::thread> waiters;
  for (int w = 0; w < kWaiters; ++w) {
    waiters.emplace_back([&, w] {
      const bool any = (w % 2) == 0;
      const bool timed = (w % 4) >= 2;
      const auto timeout =
          timed ? std::chrono::nanoseconds(30us) : std::chrono::nanoseconds(kForever);
      for (;;) {
        WaitResult result{};
        if (any) {
          std::uint32_t index = 99;
          result = wait_for_multiple_objects(both, false, timeout, &index,
                                             static_cast<std::uint32_t>(w + 1));
          if (result == WaitResult::Success) assert(index < 2u);
        } else {
          result = wait_for_single_object(first, timeout, static_cast<std::uint32_t>(w + 1));
        }
        if (result == WaitResult::Timeout) {
          assert(timed);
          continue;
        }
        assert(result == WaitResult::Success);
        // Units past the real ones are stop units, one per waiter.
        if (taken.fetch_add(1) >= kUnits) return;
        per_waiter[w].fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  std::vector<std::thread> signallers;
  for (int s = 0; s < kSignallers; ++s) {
    signallers.emplace_back([&, s] {
      for (;;) {
        const int unit = released.fetch_add(1);
        if (unit >= kUnits) break;
        assert(((unit + s) % 3 == 0 ? second : first)->release(1));
        if ((unit & 15) == 0) yield_current_thread();
      }
    });
  }
  for (auto& thread : signallers) thread.join();
  while (taken.load() < kUnits) std::this_thread::sleep_for(1ms);
  assert(first->release(kWaiters));
  for (auto& thread : waiters) thread.join();
  int real = 0;
  for (int w = 0; w < kWaiters; ++w) real += per_waiter[w].load();
  assert(real == kUnits);
  assert(taken.load() == kUnits + kWaiters);
  assert(first->count() == 0 && second->count() == 0 && "every released unit was taken once");
  assert(waiter_count(*first) == 0u && waiter_count(*second) == 0u);
  std::cout << phase << ": " << real << " units taken\n";
}

// Two threads hand control back and forth through two auto-reset events;
// returns the mean round trip. Every handoff must arrive (a lost one hangs).
std::chrono::nanoseconds ping_pong_round_trip(int round_trips) {
  auto ping = std::make_shared<KernelEvent>(false, false);
  auto pong = std::make_shared<KernelEvent>(false, false);
  Watchdog watchdog("ping-pong", std::chrono::seconds(60), [] {});
  std::thread partner([&] {
    for (int i = 0; i < round_trips; ++i) {
      assert(wait_for_single_object(ping, kForever, 2u) == WaitResult::Success);
      pong->set();
    }
  });
  const auto start = Clock::now();
  for (int i = 0; i < round_trips; ++i) {
    ping->set();
    assert(wait_for_single_object(pong, kForever, 1u) == WaitResult::Success);
  }
  const auto took = Clock::now() - start;
  partner.join();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(took) / round_trips;
}

}  // namespace

int main() {
  std::cout << std::unitbuf;
  test_zero_delay_yields();
  test_timeout_precision();
  test_spin_then_block();

  reset_wait_profile();
  set_wait_spin_limit(4us);
  stress_auto_reset_event("auto-reset event, spinning");
  stress_semaphores("semaphores, spinning");
  const auto spinning = wait_spin_counters();
  std::cout << "stress with spinning: " << spinning.registered << " registered, "
            << spinning.spins << " spun, " << spinning.spin_hits << " spin hits, "
            << spinning.blocks << " blocked\n";

  set_wait_spin_limit(0ns);
  reset_wait_profile();
  stress_auto_reset_event("auto-reset event, no spinning");
  stress_semaphores("semaphores, no spinning");
  assert(wait_spin_counters().spins == 0u);

  // Handoff latency, spinning off and on (a measurement, not a bound).
  const auto blocking = ping_pong_round_trip(5000);
  set_wait_spin_limit(4us);
  reset_wait_profile();
  const auto spinning_trip = ping_pong_round_trip(5000);
  const auto handoff = wait_spin_counters();
  std::cout << "ping-pong round trip: " << blocking.count() << " ns blocking, "
            << spinning_trip.count() << " ns with a 4 us spin (" << handoff.spin_hits
            << " spin hits, " << handoff.blocks << " blocked of " << handoff.registered
            << " registered)\n";

  std::cout << "xenon_kernel_wait_scheduling_tests: ok\n";
  return 0;
}
