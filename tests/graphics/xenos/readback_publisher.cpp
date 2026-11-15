#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

#include "xenon/gpu/readback_publisher.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/gpu_coherency.hpp"

using namespace xenon;

namespace {

// A stand-in for the GPU timeline: the publisher's thread waits on it the way
// it waits on an ID3D12Fence or a Vulkan timeline semaphore.
struct FakeTimeline {
  std::atomic<std::uint64_t> completed{0};
  bool wait(std::uint64_t value, std::uint32_t timeout_ms) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (completed.load() < value) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::yield();
    }
    return true;
  }
};

// Regression (AC6 cutscenes out of sync with their audio): every resolve was
// copied from the readback buffer into guest RAM on the GPU thread, ~20 MiB a
// frame and ~15% of a thread that also parses the ring and records every
// draw. The copy now runs on the publisher's thread as the GPU finishes each
// readback; the guest-visible wait (wait_published()) is unchanged.
void test_readbacks_land_once_the_gpu_finishes_them() {
  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());
  constexpr memory::GuestAddress kBase = 0x00400000u;
  assert(memory.commit_fixed(kBase, memory::kBasePageSize, memory::kReadWrite));
  const auto physical = memory.get_physical_address(kBase);
  assert(physical != 0xFFFFFFFFu);

  memory::GuestMemoryGpuCoherency coherency;
  coherency.reset(memory::kPhysicalMemorySize, false);
  (void)coherency.mark_gpu_write(memory.coherency(), physical, 64u);
  const auto plan = coherency.plan_readback(memory.coherency(), physical, 64u);
  assert(plan.ranges.size() == 1u);

  std::vector<std::byte> readback(4096u);
  for (std::size_t i = 0; i < 64u; ++i) readback[i] = static_cast<std::byte>(0xA0u + i);

  FakeTimeline gpu;
  gpu::ReadbackPublisher publisher;
  publisher.start(memory, coherency, readback, 16u * 1024u * 1024u,
                  [&gpu](std::uint64_t value, std::uint32_t timeout_ms) {
                    return gpu.wait(value, timeout_ms);
                  });
  publisher.push(plan.ranges[0], 0u, 5u);
  assert(publisher.pending());
  assert(publisher.last_value() == 5u);
  // Its region of the readback buffer is not free until it has landed.
  assert(publisher.published_through() == 4u);
  // A readback queued later than the value waited for is not waited for.
  assert(publisher.wait_published(4u));

  // The GPU has not finished: nothing is written.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(memory.read8(kBase) == 0u);
  assert(publisher.pending());

  std::thread finish([&gpu] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    gpu.completed.store(5u);
  });
  assert(publisher.wait_published(5u));
  finish.join();
  for (std::uint32_t i = 0; i < 64u; ++i) assert(memory.read8(kBase + i) == 0xA0u + i);
  assert(!publisher.pending());
  assert(publisher.published_through() == (std::numeric_limits<std::uint64_t>::max)());
  assert(!coherency.has_gpu_dirty(physical, 64u));
  const auto published = publisher.take_published_writes();
  assert(published.size() == 1u && published[0].address == physical && published[0].size == 64u);
  assert(publisher.take_published_writes().empty());
  assert(publisher.error().empty());
  publisher.stop();
}

// A CPU store to a resolved range while its readback is in flight is the
// newer write; the publisher writes only the bytes the GPU still owns.
void test_cpu_writes_during_the_copy_win() {
  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());
  constexpr memory::GuestAddress kBase = 0x00500000u;
  assert(memory.commit_fixed(kBase, memory::kBasePageSize, memory::kReadWrite));
  const auto physical = memory.get_physical_address(kBase);

  memory::GuestMemoryGpuCoherency coherency;
  coherency.reset(memory::kPhysicalMemorySize, false);
  (void)coherency.mark_gpu_write(memory.coherency(), physical, 32u);
  const auto plan = coherency.plan_readback(memory.coherency(), physical, 32u);
  std::vector<std::byte> readback(4096u, std::byte{0x5A});

  FakeTimeline gpu;
  gpu::ReadbackPublisher publisher;
  publisher.start(memory, coherency, readback, 16u * 1024u * 1024u,
                  [&gpu](std::uint64_t value, std::uint32_t timeout_ms) {
                    return gpu.wait(value, timeout_ms);
                  });
  publisher.push(plan.ranges[0], 0u, 1u);
  memory.write32_be(kBase + 8u, 0x11223344u);
  gpu.completed.store(1u);
  assert(publisher.wait_all());
  assert(memory.read32_be(kBase + 8u) == 0x11223344u);
  assert(memory.read8(kBase) == 0x5Au);
  assert(memory.read8(kBase + 31u) == 0x5Au);
  publisher.stop();
}

// Readbacks the GPU finishes in order land in order, several per wake, and a
// stop with work still queued (the backend tearing down) does not hang.
void test_batches_and_stop() {
  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());
  constexpr memory::GuestAddress kBase = 0x00600000u;
  assert(memory.commit_fixed(kBase, memory::kBasePageSize, memory::kReadWrite));
  const auto physical = memory.get_physical_address(kBase);

  memory::GuestMemoryGpuCoherency coherency;
  coherency.reset(memory::kPhysicalMemorySize, false);
  std::vector<std::byte> readback(4096u);
  FakeTimeline gpu;
  gpu::ReadbackPublisher publisher;
  publisher.start(memory, coherency, readback, 16u * 1024u * 1024u,
                  [&gpu](std::uint64_t value, std::uint32_t timeout_ms) {
                    return gpu.wait(value, timeout_ms);
                  });
  for (std::uint32_t i = 0; i < 8u; ++i) {
    (void)coherency.mark_gpu_write(memory.coherency(), physical + i * 16u, 16u);
    const auto plan = coherency.plan_readback(memory.coherency(), physical + i * 16u, 16u);
    for (std::uint32_t b = 0; b < 16u; ++b) readback[i * 256u + b] = static_cast<std::byte>(i + 1u);
    publisher.push(plan.ranges[0], i * 256u, 10u + i / 2u);
  }
  gpu.completed.store(11u);
  assert(publisher.wait_published(11u));
  for (std::uint32_t i = 0; i < 4u; ++i) assert(memory.read8(kBase + i * 16u) == i + 1u);
  assert(memory.read8(kBase + 4u * 16u) == 0u);
  assert(publisher.published_through() == 11u);
  assert(publisher.pending());
  publisher.stop();
  assert(!publisher.running());
}

}  // namespace

int main() {
  test_readbacks_land_once_the_gpu_finishes_them();
  test_cpu_writes_during_the_copy_win();
  test_batches_and_stop();
  std::cout << "xenon_readback_publisher_tests: ok\n";
}
