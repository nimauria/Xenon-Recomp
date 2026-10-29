#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

#include "xenon/kernel/time_stamp_bundle.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/types.hpp"

using namespace std::chrono_literals;
using xenon::kernel::TimeStampBundle;
using xenon::kernel::TimeStampBundleClock;
using xenon::memory::AddressSpace;
using xenon::memory::GuestTranslationMode;

namespace {
constexpr std::uint32_t kTickCount = 0x10u;

std::shared_ptr<AddressSpace> make_memory(xenon::cpu::GuestAddress& page) {
  auto memory = std::make_shared<AddressSpace>(GuestTranslationMode::Compact);
  const bool initialized = memory->initialize();
  assert(initialized);
  static_cast<void>(initialized);
  const bool allocated = memory->allocate(xenon::memory::kBasePageSize,
                                          xenon::memory::kBasePageSize,
                                          xenon::memory::kReadWrite, /*top_down=*/true, page);
  assert(allocated);
  static_cast<void>(allocated);
  return memory;
}

// InterruptTime counts 100 ns and TickCount milliseconds from boot; SystemTime
// is the wall clock passed through.
void test_values_count_from_boot() {
  const auto boot = std::chrono::steady_clock::time_point{} + 5s;
  const auto bundle = xenon::kernel::time_stamp_bundle_at(boot, boot + 1234567800ns, 0x01DA0000u);
  assert(bundle.interrupt_time == 12345678u);
  assert(bundle.tick_count == 1234u);
  assert(bundle.system_time == 0x01DA0000u);
  const auto before = xenon::kernel::time_stamp_bundle_at(boot, boot - 1s, 7u);
  assert(before.interrupt_time == 0u && before.tick_count == 0u);
}

// TickCount is the console's 32-bit millisecond counter, so it wraps.
void test_tick_count_wraps() {
  const auto boot = std::chrono::steady_clock::time_point{};
  const auto bundle = xenon::kernel::time_stamp_bundle_at(boot, boot + 4294967296ms + 5ms, 0u);
  assert(bundle.tick_count == 5u);
  assert(bundle.interrupt_time == (4294967296ull + 5u) * 10000u);
}

// The guest layout: big-endian u64 InterruptTime, u64 SystemTime, u32 TickCount,
// u32 padding.
void test_guest_layout() {
  xenon::cpu::GuestAddress page{};
  auto memory = make_memory(page);
  memory->write32_be(page + 0x14u, 0xFFFFFFFFu);
  TimeStampBundle bundle{};
  bundle.interrupt_time = 0x0102030405060708ull;
  bundle.system_time = 0x1112131415161718ull;
  bundle.tick_count = 0x21222324u;
  xenon::kernel::write_time_stamp_bundle(*memory, page, bundle);
  assert(memory->read8(page + 0x00u) == 0x01u && memory->read8(page + 0x07u) == 0x08u);
  assert(memory->read64_be(page + 0x08u) == 0x1112131415161718ull);
  assert(memory->read32_be(page + kTickCount) == 0x21222324u);
  assert(memory->read32_be(page + 0x14u) == 0u);
}

// AC6's BGM thread steps its music fade-in by the TickCount difference between
// passes; with a bundle that never changed the music stayed silent. A guest
// polling the bundle must see it follow host time.
void test_clock_keeps_bundle_current() {
  xenon::cpu::GuestAddress page{};
  auto memory = make_memory(page);
  TimeStampBundleClock clock{};
  const auto started = std::chrono::steady_clock::now();
  assert(clock.start(memory, page));
  assert(clock.running());
  const auto first = memory->read32_be(page + kTickCount);
  assert(first <= 5u);
  assert(memory->read64_be(page + 0x08u) != 0u);  // SystemTime is set at once

  std::this_thread::sleep_for(100ms);
  const auto tick = memory->read32_be(page + kTickCount);
  const auto interrupt = memory->read64_be(page);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  std::cout << "after " << elapsed << " ms: TickCount=" << tick << '\n';
  assert(tick >= 90u && tick <= static_cast<std::uint32_t>(elapsed));
  // Both counters come from one sample: InterruptTime agrees with TickCount
  // to the millisecond.
  assert(interrupt / 10000u >= tick && interrupt / 10000u <= tick + 1u);

  clock.stop();
  assert(!clock.running());
  const auto stopped = memory->read32_be(page + kTickCount);
  std::this_thread::sleep_for(20ms);
  assert(memory->read32_be(page + kTickCount) == stopped);
}

// Restarting begins a new boot; an unwritable bundle is refused.
void test_restart_and_refusal() {
  xenon::cpu::GuestAddress page{};
  auto memory = make_memory(page);
  TimeStampBundleClock clock{};
  assert(clock.start(memory, page));
  std::this_thread::sleep_for(30ms);
  assert(clock.start(memory, page));
  assert(memory->read32_be(page + kTickCount) <= 5u);
  clock.stop();

  assert(!clock.start(memory, 0x00001000u));
  assert(!clock.running());
  assert(!clock.start(nullptr, page));
}
}  // namespace

int main() {
  test_values_count_from_boot();
  test_tick_count_wraps();
  test_guest_layout();
  test_clock_keeps_bundle_current();
  test_restart_and_refusal();
  std::cout << "time stamp bundle tests passed\n";
  return 0;
}
