// HostSampler: a busy thread is sampled as running, a blocked one as waiting,
// and each sampled thread keeps its name; addresses resolve to a symbol or a
// module offset, never a bare number for code in this executable.

#include "xenon/logging/host_sampler.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace {

std::atomic<bool> g_stop{false};
std::atomic<std::uint64_t> g_sink{0};

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void sampler_test_busy_loop() {
  std::uint64_t value = 1;
  while (!g_stop.load(std::memory_order_relaxed)) {
    for (int i = 0; i < 1000; ++i) value = value * 6364136223846793005ull + 1442695040888963407ull;
    g_sink.store(value, std::memory_order_relaxed);
  }
}

const xenon::logging::HostThreadProfile* find(
    const std::vector<xenon::logging::HostThreadProfile>& profiles, const std::string& name) {
  for (const auto& profile : profiles)
    if (profile.name == name) return &profile;
  return nullptr;
}

}  // namespace

int main() {
  using xenon::logging::HostSampler;
  if (!HostSampler::supported()) {
    std::puts("host sampler unsupported on this host: skipped");
    return 0;
  }

  // The environment switch.
  assert(!xenon::logging::host_sampler_options_from_environment() ||
         std::getenv("XENON_HOST_PROFILE") != nullptr);

  std::mutex mutex;
  std::condition_variable wake;
  bool released = false;
  std::thread busy([] {
    xenon::logging::set_current_thread_name("sampler-busy");
    sampler_test_busy_loop();
  });
  std::thread blocked([&] {
    xenon::logging::set_current_thread_name("sampler-blocked");
    std::unique_lock lock(mutex);
    wake.wait(lock, [&] { return released; });
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  HostSampler sampler;
  xenon::logging::HostSamplerOptions options{};
  options.interval = std::chrono::microseconds(1000);
  options.min_thread_samples = 1;
  assert(sampler.start(options));
  assert(!sampler.start(options));  // already running
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  // A report while sampling continues (the periodic snapshot a killed run
  // keeps) already names the busy thread, and sampling goes on after it.
  {
    const auto partial = sampler.report();
    assert(partial.find("\"sampler-busy\"") != std::string::npos);
    const auto before = sampler.samples();
    assert(sampler.running());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(sampler.samples() > before + 20u);
  }
  sampler.stop();
  assert(!sampler.running());
  assert(sampler.samples() > 50u);

  g_stop = true;
  {
    std::scoped_lock lock(mutex);
    released = true;
  }
  wake.notify_all();
  busy.join();
  blocked.join();

  const auto profiles = sampler.profiles();
  const auto* busy_profile = find(profiles, "sampler-busy");
  const auto* blocked_profile = find(profiles, "sampler-blocked");
  assert(busy_profile && blocked_profile);
  assert(busy_profile->samples > 50u && blocked_profile->samples > 50u);
  const double busy_running =
      double(busy_profile->running_samples) / double(busy_profile->samples);
  const double blocked_running =
      double(blocked_profile->running_samples) / double(blocked_profile->samples);
  std::printf("busy ran in %.0f%% of samples, blocked in %.0f%%\n", busy_running * 100.0,
              blocked_running * 100.0);
  assert(busy_running > 0.3);
  assert(blocked_running < 0.2);
  assert(busy_profile->cpu_ns > 100'000'000u);  // at least 0.1 s of the 0.6 s
  assert(!busy_profile->functions.empty());
  // The busy thread's heaviest function is its loop, by name where symbols
  // exist, and in this executable either way.
  const auto top = busy_profile->functions.front().symbol;
  std::printf("busy thread's top function: %s\n", top.c_str());
  assert(top.rfind("0x", 0) != 0);

  const auto self = HostSampler::symbolize(reinterpret_cast<std::uintptr_t>(&sampler_test_busy_loop));
  std::printf("symbolized: %s\n", self.c_str());
  assert(!self.empty() && self.rfind("0x", 0) != 0);

  const auto report = sampler.report();
  assert(report.find("\"sampler-busy\"") != std::string::npos);
  assert(report.find("host profile: interval 1000 us") == 0);
  std::puts("host sampler tests passed");
  return 0;
}
