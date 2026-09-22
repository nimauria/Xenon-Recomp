#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>

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
