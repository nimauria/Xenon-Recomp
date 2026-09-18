#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

#include "xenon/memory/address_space.hpp"

using namespace xenon::memory;

static bool faults(auto&& fn, FaultReason reason) {
  try {
    fn();
  } catch (const MemoryFault& e) {
    return e.reason() == reason;
  }
  return false;
}

int main() {
  AddressSpace mem;
  assert(mem.initialize());

  auto rs = AddressSpace::regions();
  assert(rs.size() == 9);
  assert(rs[0].base == 0x00000000u && rs[0].allocation_page_size == 0x1000u);
  assert(rs[1].base == 0x40000000u && rs[1].allocation_page_size == 0x10000u);
  assert(rs[5].base == 0xA0000000u && rs[5].kind == RegionKind::PhysicalAlias);

  assert(faults([&] { (void)mem.read8(0); }, FaultReason::Uncommitted));

  // 4 KiB virtual mapping backed by real 512 MiB physical RAM.
  assert(mem.commit_fixed(0x00100000u, 0x2000u, kReadWrite));
  mem.write32_be(0x00100FFEu, 0x11223344u);  // intentionally crosses a page
  assert(mem.read32_be(0x00100FFEu) == 0x11223344u);
  mem.write32_le(0x00100020u, 0xA1B2C3D4u);
  assert(mem.read32_le(0x00100020u) == 0xA1B2C3D4u);
  assert(mem.read32_be(0x00100020u) == 0xD4C3B2A1u);

  const auto phys = mem.get_physical_address(0x00100000u);
  assert(phys != 0xFFFFFFFFu);
  const auto physical_alias = 0xA0000000u + phys;
  mem.write32_be(physical_alias, 0xCAFEBABEu);
  assert(mem.read32_be(0x00100000u) == 0xCAFEBABEu);
  mem.write32_be(0x00100004u, 0x01234567u);
  assert(mem.read32_be(0xA0000000u + mem.get_physical_address(0x00100004u)) == 0x01234567u);

  // Memory V2: generated PPC acquires one concrete access context and common
  // aligned RAM operations translate through the compact hot page table.
  auto fast = mem.access_context();
  assert(fast.has_fast_path());
  const auto coherency_before = mem.coherency().current_epoch();
  fast.write32_be(0x00100040u, 0xAABBCCDDu);
  assert(fast.read32_be(0x00100040u) == 0xAABBCCDDu);
  const auto coherency_after = mem.coherency().current_epoch();
  assert(coherency_after > coherency_before);
  assert(mem.coherency().range_changed_since(
      mem.get_physical_address(0x00100040u), sizeof(std::uint32_t),
      coherency_before, coherency_after));

  // Six Xenon hardware-thread shaped stress: disjoint aligned words share the
  // same guest mapping while normal RAM access remains lock-free at AddressSpace.
  std::array<std::thread, 6> workers{};
  for (std::uint32_t thread = 0; thread < workers.size(); ++thread) {
    workers[thread] = std::thread([&, thread] {
      auto access = mem.access_context();
      const GuestAddress address = 0x00100200u + thread * 64u;
      for (std::uint32_t i = 0; i < 2000u; ++i) {
        access.write64_be(address, (std::uint64_t{thread} << 32u) | i);
        assert(access.read64_be(address) ==
               ((std::uint64_t{thread} << 32u) | i));
      }
    });
  }
  for (auto& worker : workers) worker.join();

  // Range operations use page-chunked native copies/fills. Overlap keeps the
  // old snapshot semantics rather than degenerating into byte-at-a-time calls.
  for (std::uint32_t i = 0; i < 64u; ++i) fast.write8(0x00100400u + i, i);
  mem.copy(0x00100408u, 0x00100400u, 48u);
  for (std::uint32_t i = 0; i < 48u; ++i) {
    assert(fast.read8(0x00100408u + i) == i);
  }
  mem.fill(0x00100FF0u, 32u, 0x5Au);
  for (std::uint32_t i = 0; i < 32u; ++i) {
    assert(fast.read8(0x00100FF0u + i) == 0x5Au);
  }

  // The dedicated 0x7F GPU/writeback view aliases physical RAM at 0.
  mem.write32_be(0x7F000100u, 0xDEADC0DEu);
  assert(mem.read32_be(0xA0000100u) == 0xDEADC0DEu);

  // E-view begins one base page into physical memory.
  mem.write32_be(0xE0000000u, 0x55667788u);
  assert(mem.read32_be(0xA0001000u) == 0x55667788u);

  // The two XEX windows are aliases of one logical image mapping.
  assert(mem.commit_fixed(0x82000000u, 0x10000u, kReadWriteExecute));
  mem.write32_be(0x82001234u, 0x0BADF00Du);
  assert(mem.fetch32_be(0x82001234u) == 0x0BADF00Du);
  assert(mem.read32_be(0x92001234u) == 0x0BADF00Du);
  assert(mem.get_physical_address(0x82001234u) == mem.get_physical_address(0x92001234u));

  // Protection is enforced by CPU-visible accesses.
  Protect old{};
  assert(mem.protect(0x00100000u, 0x1000u, Protect::Read, &old));
  assert(faults([&] { (void)mem.fetch32_be(0x00100000u); }, FaultReason::Protection));
  assert(has(old, Protect::Write));
  assert(faults([&] { mem.write8(0x00100000u, 1); }, FaultReason::Protection));
  assert(mem.read32_be(0x00100000u) == 0xCAFEBABEu);
  assert(mem.protect(0x00100000u, 0x1000u, kReadWrite));

  // Reservations are physical: a write through another alias invalidates it.
  std::uint32_t reserved{};
  const auto token = mem.reserve32(0x00100000u, reserved);
  assert(reserved == 0xCAFEBABEu);
  mem.write8(physical_alias + 8u, 0x42u);  // same 128-byte Xenon cache/reservation granule
  assert(!mem.store_conditional32(0x00100000u, token, 0x11111111u));
  // A write outside the reservation granule does not invalidate it.
  const auto token_far = mem.reserve32(0x00100000u, reserved);
  mem.write8(physical_alias + 0x180u, 0x24u);
  assert(mem.store_conditional32(0x00100000u, token_far, 0x18181818u));
  // A token from another physical granule is never valid for this address.
  std::uint32_t other_value{};
  const auto other_token = mem.reserve32(0x00100180u, other_value);
  assert(!mem.store_conditional32(0x00100000u, other_token, 0x19191919u));
  const auto token2 = mem.reserve32(0x00100000u, reserved);
  assert(mem.store_conditional32(0x00100000u, token2, 0x22222222u));
  assert(mem.read32_be(physical_alias) == 0x22222222u);

  // Explicit physical allocation can be mapped into a virtual range for GPU/APU sharing.
  std::uint32_t shared_phys{};
  assert(mem.allocate_physical(0x2000u, 0x1000u, false, shared_phys));
  assert(mem.map_virtual_to_physical(0x00200000u, shared_phys, 0x2000u, kReadWrite));
  mem.write32_be(0x00200000u, 0x31415926u);
  assert(mem.read32_be(0xA0000000u + shared_phys) == 0x31415926u);
  // Releasing only the virtual view must not free the owned physical allocation.
  assert(!mem.free_physical(shared_phys, 0x2000u));
  assert(mem.release(0x00200000u));
  assert(mem.read32_be(0xA0000000u + shared_phys) == 0x31415926u);
  assert(mem.free_physical(shared_phys, 0x2000u));

  // Memory V2 physical ownership: an anonymous page may outlive its owning
  // virtual allocation while another guest mapping still aliases it. It must
  // not return to the physical allocator until the final mapping disappears.
  {
    AddressSpace ownership_mem;
    assert(ownership_mem.initialize());
    constexpr GuestAddress owner_va = 0x00100000u;
    constexpr GuestAddress alias_va = 0x00200000u;
    assert(ownership_mem.commit_fixed(owner_va, kBasePageSize, kReadWrite));
    const auto owner_phys = ownership_mem.get_physical_address(owner_va);
    assert(owner_phys != 0xFFFFFFFFu);
    assert(ownership_mem.map_virtual_to_physical(
        alias_va, owner_phys, kBasePageSize, kReadWrite));
    ownership_mem.write32_be(owner_va, 0x44556677u);
    assert(ownership_mem.release(owner_va));
    assert(ownership_mem.read32_be(alias_va) == 0x44556677u);

    std::uint32_t while_aliased{};
    assert(ownership_mem.allocate_physical(kBasePageSize, kBasePageSize,
                                           false, while_aliased));
    assert(while_aliased != owner_phys);
    assert(ownership_mem.release(alias_va));
    assert(ownership_mem.free_physical(while_aliased, kBasePageSize));

    std::uint32_t after_alias_release{};
    assert(ownership_mem.allocate_physical(kBasePageSize, kBasePageSize,
                                           false, after_alias_release));
    assert(after_alias_release == owner_phys);
    assert(ownership_mem.free_physical(after_alias_release, kBasePageSize));
  }

  // 64 KiB virtual allocations use the second guest heap and preserve alignment.
  GuestAddress large_alloc{};
  assert(mem.allocate(0x18000u, 0x10000u, kReadWrite, true, large_alloc, kLargePageSize));
  assert((large_alloc & 0xFFFFu) == 0 && large_alloc >= kVirtual64KBase && large_alloc <= kVirtual64KEnd);
  auto large_q = mem.query(large_alloc);
  assert(large_q && large_q->page_size == kLargePageSize && large_q->allocation_size == 0x20000u);
  assert(mem.release(large_alloc));

  // Shared-memory observers and reservation invalidation also see direct DMA writes.
  std::uint32_t observed_phys = 0xFFFFFFFFu, observed_width = 0;
  const auto write_observer = mem.add_physical_write_callback(
      [&](std::uint32_t p, std::uint32_t w) { observed_phys = p; observed_width = w; });
  const auto token_dma = mem.reserve32(0x00100000u, reserved);
  auto* raw = mem.physical_data(phys);
  assert(raw != nullptr);
  raw[0] = std::byte{0xAA};
  mem.notify_external_write(phys, 1);
  std::array<std::byte, 4> snapshot{};
  assert(mem.copy_physical_range(phys, snapshot));
  assert(snapshot[0] == std::byte{0xAA});
  assert(!mem.copy_physical_range(kPhysicalMemorySize - 1u, snapshot));
  assert(observed_phys == phys && observed_width == 1);
  assert(!mem.store_conditional32(0x00100000u, token_dma, 0xAAAAAAAAu));
  mem.remove_physical_write_callback(write_observer);

  // A CPU store crossing virtually adjacent but physically discontiguous
  // pages must dirty both physical ranges independently for GPU mirrors.
  constexpr GuestAddress split_virtual = 0x00300000u;
  constexpr std::uint32_t split_physical_a = 0x04000000u;
  constexpr std::uint32_t split_physical_b = 0x08000000u;
  assert(mem.map_virtual_to_physical(split_virtual, split_physical_a,
                                     kBasePageSize, kReadWrite));
  assert(mem.map_virtual_to_physical(split_virtual + kBasePageSize,
                                     split_physical_b, kBasePageSize,
                                     kReadWrite));
  std::vector<std::pair<std::uint32_t, std::uint32_t>> split_notifications;
  const auto split_observer = mem.add_physical_write_callback(
      [&](std::uint32_t p, std::uint32_t w) {
        split_notifications.emplace_back(p, w);
      });
  mem.write32_be(split_virtual + kBasePageSize - 2u, 0x12345678u);
  assert((split_notifications ==
          std::vector<std::pair<std::uint32_t, std::uint32_t>>{
              {split_physical_a + kBasePageSize - 2u, 2u},
              {split_physical_b, 2u}}));
  split_notifications.clear();
  mem.fill(split_virtual + kBasePageSize - 2u, 4u, 0xA5u);
  assert((split_notifications ==
          std::vector<std::pair<std::uint32_t, std::uint32_t>>{
              {split_physical_a + kBasePageSize - 2u, 2u},
              {split_physical_b, 2u}}));
  mem.remove_physical_write_callback(split_observer);
  assert(mem.release(split_virtual));
  assert(mem.release(split_virtual + kBasePageSize));
  assert(mem.free_physical(split_physical_a, kBasePageSize));
  assert(mem.free_physical(split_physical_b, kBasePageSize));

  // MMIO can override any guest virtual range, as real Xbox devices do.
  std::uint32_t mmio_last = 0;
  assert(mem.add_mmio_range(
      0x7FEA0000u, 0x10000u,
      [&](GuestAddress a, std::uint32_t width) -> std::uint64_t {
        assert(a == 0x7FEA0010u && width == 4);
        return 0x12345678u;
      },
      [&](GuestAddress a, std::uint32_t width, std::uint64_t value) {
        assert(a == 0x7FEA0010u && width == 4);
        mmio_last = static_cast<std::uint32_t>(value);
      },
      "test-device"));
  assert(mem.read32_be(0x7FEA0010u) == 0x12345678u);
  // The hot page marks MMIO as slow, so the concrete context dispatches to the
  // existing device semantics rather than treating the physical alias as RAM.
  auto mmio_access = mem.access_context();
  assert(mmio_access.read32_be(0x7FEA0010u) == 0x12345678u);
  mmio_access.write32_be(0x7FEA0010u, 0x89ABCDEFu);
  assert(mmio_last == 0x89ABCDEFu);

  std::uint32_t invalidated = 0;
  const auto callback_id = mem.add_invalidation_callback([&](GuestAddress a) { invalidated = a; });
  mem.instruction_cache_invalidate(0x82001234u);
  assert(invalidated == 0x82001234u);
  mem.remove_invalidation_callback(callback_id);

  // Allocation / decommit / release state is queryable for the future kernel layer.
  GuestAddress allocated{};
  assert(mem.allocate(0x3000u, 0x1000u, kReadWrite, false, allocated));
  auto q = mem.query(allocated);
  assert(q && q->state == PageState::Committed && q->allocation_size == 0x3000u);
  assert(mem.decommit(allocated, 0x1000u));
  q = mem.query(allocated);
  assert(q && q->state == PageState::Reserved);
  assert(mem.release(allocated));
  q = mem.query(allocated);
  assert(q && q->state == PageState::Free);

  // Whole-address-space reset is observable by an attached native GPU mirror.
  std::uint32_t reset_address = 0xFFFFFFFFu, reset_width = 0;
  const auto reset_observer = mem.add_physical_write_callback(
      [&](std::uint32_t p, std::uint32_t w) {
        reset_address = p;
        reset_width = w;
      });
  mem.reset();
  assert(reset_address == 0 && reset_width == kPhysicalMemorySize);
  mem.remove_physical_write_callback(reset_observer);

  std::cout << "xenon_memory_tests: ok\n";
}
