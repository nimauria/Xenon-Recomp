#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/types.hpp"

using xenon::kernel::GuestHeapManager;
using xenon::kernel::KernelMemory;
using xenon::kernel::KernelProcess;
using xenon::memory::AddressSpace;
using xenon::memory::GuestTranslationMode;
using xenon::memory::MappingInfo;
using xenon::memory::PageState;

int main() {
  auto address_space = std::make_shared<AddressSpace>(GuestTranslationMode::Compact);
  assert(address_space->initialize());
  auto memory = std::make_shared<KernelMemory>(address_space);
  KernelProcess process(memory);
  auto& heap = process.guest_heap();

  constexpr std::uint32_t kHeapA = 0x10001000u;
  constexpr std::uint32_t kHeapB = 0x20002000u;

  const auto a = heap.allocate(kHeapA, 0, 37);
  assert(a != 0 && (a & 0xFu) == 0u);
  assert(heap.size(kHeapA, 0, a) == 37u);
  assert(heap.owns(kHeapA, a));
  assert(!heap.owns(kHeapB, a));

  std::array<std::byte, 37> pattern{};
  for (std::size_t i = 0; i < pattern.size(); ++i)
    pattern[i] = static_cast<std::byte>((i * 7u) & 0xFFu);
  assert(memory->write_bytes(a, pattern));
  std::array<std::byte, 37> readback{};
  assert(memory->read_bytes(a, readback));
  assert(readback == pattern);

  const auto grown = heap.reallocate(kHeapA, 0, a, 0x20000u);
  assert(grown != 0);
  assert(heap.size(kHeapA, 0, grown) == 0x20000u);
  std::array<std::byte, 37> grown_readback{};
  assert(memory->read_bytes(grown, grown_readback));
  assert(grown_readback == pattern);

  const auto shrunk = heap.reallocate(kHeapA, 0, grown, 13u);
  assert(shrunk != 0);
  assert(heap.size(kHeapA, 0, shrunk) == 13u);
  std::array<std::byte, 13> shrunk_readback{};
  assert(memory->read_bytes(shrunk, shrunk_readback));
  for (std::size_t i = 0; i < shrunk_readback.size(); ++i)
    assert(shrunk_readback[i] == pattern[i]);

  const auto zero = heap.allocate(kHeapA, GuestHeapManager::kHeapZeroMemory, 0u);
  assert(zero != 0);
  // Xbox/ReXCRT normalizes a zero-byte request to a one-byte allocation.
  assert(heap.size(kHeapA, 0, zero) == 1u);
  assert(address_space->read8(zero) == 0u);

  const auto zeroed = heap.allocate(kHeapA, GuestHeapManager::kHeapZeroMemory, 128u);
  assert(zeroed != 0);
  for (std::uint32_t i = 0; i < 128u; ++i) assert(address_space->read8(zeroed + i) == 0u);

  assert(heap.size(kHeapB, 0, shrunk) == GuestHeapManager::kInvalidSize);
  assert(!heap.free(kHeapB, 0, shrunk));
  assert(!heap.free(kHeapA, 0, 0x12345678u));

  const auto reusable = heap.allocate(kHeapA, 0, 64u);
  assert(reusable != 0);
  assert(heap.free(kHeapA, 0, reusable));
  assert(!heap.free(kHeapA, 0, reusable));
  const auto replacement = heap.allocate(kHeapA, 0, 64u);
  assert(replacement != 0);

  std::array<std::uint32_t, 32> fragmented{};
  for (auto& address : fragmented) {
    address = heap.allocate(kHeapA, 0, 96u);
    assert(address != 0);
  }
  for (std::size_t i = 0; i < fragmented.size(); i += 2u)
    assert(heap.free(kHeapA, 0, fragmented[i]));
  for (std::size_t i = 0; i < fragmented.size() / 2u; ++i)
    assert(heap.allocate(kHeapA, 0, 80u) != 0);

  // A real RTL heap sub-allocates small blocks out of larger committed
  // segments, so a title can hold hundreds of thousands of small CRT
  // allocations live at once. Regression: every allocation used to take its
  // own 64 KiB-aligned virtual region, exhausting the guest's virtual range
  // after a few thousand small blocks (AC6's per-thread CRT data calloc of
  // 0xC4 bytes then failed and _getptd recursed until the stack overflowed).
  {
    constexpr std::uint32_t kHeapC = 0x40000000u;
    constexpr std::uint32_t kSmallCount = 200000u;
    const auto segments_before = heap.segment_count();
    std::vector<std::uint32_t> blocks;
    blocks.reserve(kSmallCount);
    for (std::uint32_t i = 0; i < kSmallCount; ++i) {
      const auto block = heap.allocate(kHeapC, GuestHeapManager::kHeapZeroMemory, 0xC4u);
      if (block == 0) {
        std::cerr << "small heap blocks served before failure: " << i << "\n";
        assert(false && "guest heap ran out serving small blocks");
      }
      assert((block % GuestHeapManager::kBlockAlignment) == 0u);
      assert(heap.owns(kHeapC, block));
      assert(!heap.owns(kHeapA, block));
      blocks.push_back(block);
    }
    // Every block is distinct and none overlap (0xC4 rounds to 0xD0).
    auto sorted = blocks;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t i = 1; i < sorted.size(); ++i) assert(sorted[i] - sorted[i - 1] >= 0xC4u);
    // Zeroed and independently writable.
    assert(address_space->read32_be(blocks.back() + 0xC0u) == 0u);
    address_space->write32_be(blocks[0], 0xDEADBEEFu);
    assert(address_space->read32_be(blocks[1]) == 0u);
    // Sub-allocated: ~40 MiB of blocks lives in ~40 1 MiB segments, not 200k
    // separate 64 KiB virtual allocations.
    const auto grown_segments = heap.segment_count() - segments_before;
    assert(grown_segments <= (kSmallCount * 0xD0u) / GuestHeapManager::kSegmentSize + 2u);
    // Committed heap memory comes out of the shared 512 MiB physical pool, so
    // small blocks must cost physical RAM proportional to their size. The old
    // one-64KiB-page-per-block heap drained physical memory and starved GPU
    // and audio physical allocations (AC6 PHYS_ALLOC_FAIL with 525 pages free).
    {
      std::uint32_t physical = 0;
      assert(address_space->allocate_physical(256u * 1024u * 1024u, 4096u, false, physical));
      assert(address_space->free_physical(physical, 256u * 1024u * 1024u));
    }

    // Freeing everything coalesces each segment back to one run and returns
    // all but one of this heap's segments to the virtual allocator.
    for (const auto block : blocks) assert(heap.free(kHeapC, 0, block));
    assert(heap.segment_count() == segments_before + 1u);
    // The coalesced segment serves a near-segment-sized block without growing.
    const auto big = heap.allocate(kHeapC, 0, GuestHeapManager::kLargeThreshold);
    assert(big != 0);
    assert(heap.segment_count() == segments_before + 1u);
    assert(heap.free(kHeapC, 0, big));

    // Repeated alloc/free cycles reuse freed space instead of leaking it.
    for (int cycle = 0; cycle < 10000; ++cycle) {
      const auto block = heap.allocate(kHeapC, 0, 0x100u);
      assert(block != 0);
      assert(heap.free(kHeapC, 0, block));
    }
    assert(heap.segment_count() == segments_before + 1u);

    // Reallocate shrinks and grows in place inside a segment, preserving data.
    const auto r = heap.allocate(kHeapC, 0, 0x40u);
    assert(r != 0);
    address_space->write32_be(r, 0x11223344u);
    assert(heap.reallocate(kHeapC, 0, r, 0x20u) == r);
    assert(heap.size(kHeapC, 0, r) == 0x20u);
    const auto r_grown = heap.reallocate(kHeapC, GuestHeapManager::kHeapZeroMemory, r, 0x400u);
    assert(r_grown == r);
    assert(heap.size(kHeapC, 0, r) == 0x400u);
    assert(address_space->read32_be(r) == 0x11223344u);
    assert(address_space->read32_be(r + 0x3FCu) == 0u);
    // Growing past the large-block threshold moves it to a dedicated
    // allocation and keeps the contents.
    const auto r_large = heap.reallocate(kHeapC, 0, r, GuestHeapManager::kLargeThreshold + 1u);
    assert(r_large != 0 && r_large != r);
    assert(address_space->read32_be(r_large) == 0x11223344u);
    assert(!heap.owns(kHeapC, r));
    assert(heap.free(kHeapC, 0, r_large));
  }

  // XamAlloc's heap identity is isolated from title heaps and the default
  // process heap, and its blocks are sub-allocated like any other heap's.
  {
    constexpr auto kXam = GuestHeapManager::kXamHeapHandle;
    const auto segments_before = heap.segment_count();
    std::vector<std::uint32_t> xam_blocks;
    for (int i = 0; i < 5000; ++i) {
      const auto block = heap.allocate(kXam, GuestHeapManager::kHeapZeroMemory, 0x30u);
      assert(block != 0);
      assert(address_space->read32_be(block) == 0u);
      xam_blocks.push_back(block);
    }
    assert(heap.segment_count() == segments_before + 1u);
    assert(!heap.owns(0u, xam_blocks[0]));
    assert(!heap.free(0u, 0, xam_blocks[0]));
    assert(!heap.free(kHeapA, 0, xam_blocks[0]));
    for (const auto block : xam_blocks) assert(heap.free(kXam, 0, block));
  }

  // A request far larger than the Xbox virtual allocator can satisfy fails
  // cleanly without returning a host pointer or corrupting existing state.
  assert(heap.allocate(kHeapA, 0, 0xF0000000u) == 0u);

  const auto cleanup_address = heap.allocate(kHeapA, 0, 4096u);
  assert(cleanup_address != 0);
  assert(heap.outstanding_allocations() != 0u);
  process.terminate(0);
  assert(heap.outstanding_allocations() == 0u);
  MappingInfo info{};
  assert(memory->query_virtual(cleanup_address, info));
  assert(info.state == PageState::Free);

  std::cout << "Guest heap tests passed\n";
  return 0;
}
