// Phase 1/4 (AC6 Runtime Readiness pass): xboxkrnl guest timebase exports.
// Drives each export through core::ExportRegistry::invoke() exactly as a
// guest thunk would (real ordinal, real ExportCallContext), not by calling
// the handler C++ function directly.

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/time.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xboxkrnl_time_exports.hpp"

using namespace xenon;

namespace {

void test_ordinals_are_registered() {
  core::ExportRegistry registry;
  assert(xbox::register_xboxkrnl_time_exports(registry));

  assert(registry.contains("xboxkrnl.exe", 0x083u));
  assert(registry.contains("xboxkrnl", 0x083u));  // ".exe" normalization
  assert(registry.contains("xboxkrnl", "KeQueryPerformanceFrequency"));
  assert(registry.contains("xboxkrnl", 0x084u));
  assert(registry.contains("xboxkrnl", "KeQuerySystemTime"));
  assert(registry.contains("xboxkrnl", 0x05Au));
  assert(registry.contains("xboxkrnl", "KeDelayExecutionThread"));
  assert(registry.contains("xboxkrnl", 0x0A8u));
  assert(registry.contains("xboxkrnl", "KeStallExecutionProcessor"));

  // Registering twice must remain safe (session re-init calls this again).
  assert(xbox::register_xboxkrnl_time_exports(registry));
}

void test_ke_query_performance_frequency() {
  core::ExportRegistry registry;
  assert(xbox::register_xboxkrnl_time_exports(registry));

  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());
  cpu::CpuState cpu{};
  core::ExportCallContext call{cpu, memory, 0, 0};

  const auto result = registry.invoke("xboxkrnl", 0x083u, call);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == kernel::TimeServices::performance_frequency());
  // The real, fixed hardware value (verified against xenia-project/xenia,
  // not guessed - see kGuestTimeBaseFrequencyHz's doc comment), not just a
  // tautological self-comparison: a regression back to an arbitrary value
  // (e.g. a nanosecond-scale 1000000000) must fail this test.
  assert(cpu.gpr[3] == 50000000u &&
         "KeQueryPerformanceFrequency must report the real Xbox 360 50MHz "
         "time-base rate");
}

// Regression: performance_counter() previously had no relationship to real
// elapsed time (XenonSession::read_time_base() used its own unrelated
// increment-by-one-per-call counter instead). It must now be monotonic and
// advance at the real, documented 50MHz rate within a generous scheduling
// tolerance - not merely "some number that changes".
void test_performance_counter_is_monotonic_and_tracks_real_time() {
  const auto before = kernel::TimeServices::performance_counter();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto after = kernel::TimeServices::performance_counter();

  assert(after > before && "performance_counter() must never go backward");

  const auto delta_ticks = after - before;
  const auto delta_ms =
      (delta_ticks * 1000u) / kernel::TimeServices::performance_frequency();
  // Sleeping 50ms should read back as roughly 50 guest-ticks-derived ms;
  // generous bounds absorb real scheduler jitter without masking a real
  // unit/scaling bug (which would be off by orders of magnitude, not a few
  // ms).
  assert(delta_ms >= 30u && delta_ms <= 500u &&
         "performance_counter() delta must track real elapsed wall-clock "
         "time at the real 50MHz rate");
}

void test_ke_query_system_time_writes_guest_memory() {
  core::ExportRegistry registry;
  assert(xbox::register_xboxkrnl_time_exports(registry));

  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());
  memory::GuestAddress time_out{};
  assert(memory.allocate(8, 8, memory::kReadWrite, false, time_out));

  cpu::CpuState cpu{};
  cpu.gpr[3] = time_out;
  core::ExportCallContext call{cpu, memory, 0, 0};

  const auto before = kernel::TimeServices::system_time();
  const auto result = registry.invoke("xboxkrnl", 0x084u, call);
  assert(result.handled && result.success);
  const auto after = kernel::TimeServices::system_time();

  const std::uint64_t written = memory.read64_be(time_out);
  assert(written >= before && written <= after);
}

void test_ke_delay_execution_thread_relative_blocks_calling_thread() {
  core::ExportRegistry registry;
  assert(xbox::register_xboxkrnl_time_exports(registry));

  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());
  memory::GuestAddress interval{};
  assert(memory.allocate(8, 8, memory::kReadWrite, false, interval));

  // Relative 50ms delay: negative 100ns units.
  constexpr std::int64_t kDelayMs = 50;
  const std::int64_t raw = -(kDelayMs * 10'000);
  memory.write64_be(interval, static_cast<std::uint64_t>(raw));

  cpu::CpuState cpu{};
  cpu.gpr[3] = 0;         // processor mode (ignored)
  cpu.gpr[4] = 0;         // alertable (ignored)
  cpu.gpr[5] = interval;  // PLARGE_INTEGER Interval
  core::ExportCallContext call{cpu, memory, 0, 0};

  const auto start = std::chrono::steady_clock::now();
  const auto result = registry.invoke("xboxkrnl", 0x05Au, call);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0u);  // STATUS_SUCCESS
  assert(elapsed >= std::chrono::milliseconds(kDelayMs - 5));
}

void test_ke_stall_execution_processor_blocks_for_microseconds() {
  core::ExportRegistry registry;
  assert(xbox::register_xboxkrnl_time_exports(registry));

  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());
  cpu::CpuState cpu{};
  cpu.gpr[3] = 20'000;  // 20ms
  core::ExportCallContext call{cpu, memory, 0, 0};

  const auto start = std::chrono::steady_clock::now();
  const auto result = registry.invoke("xboxkrnl", 0x0A8u, call);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  assert(result.handled && result.success);
  assert(elapsed >= std::chrono::milliseconds(15));
}

}  // namespace

int main() {
  std::cout << "Testing xboxkrnl time exports...\n";

  test_ordinals_are_registered();
  test_ke_query_performance_frequency();
  test_performance_counter_is_monotonic_and_tracks_real_time();
  test_ke_query_system_time_writes_guest_memory();
  test_ke_delay_execution_thread_relative_blocks_calling_thread();
  test_ke_stall_execution_processor_blocks_for_microseconds();

  std::cout << "All xboxkrnl time export tests passed!\n";
  return 0;
}
