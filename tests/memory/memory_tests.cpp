#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

#include "xenon/cpu/memory_ordering.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/host_vm.hpp"

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
  // Host VM lifecycle stays below the Xbox memory model. Validate the common
  // reserve/commit/protect/decommit/discard contract on the active platform.
  const auto host_page = host_vm::page_size();
  const auto host_granularity = host_vm::allocation_granularity();
  assert(host_page && std::has_single_bit(host_page));
  assert(host_granularity >= host_page &&
         (host_granularity % host_page) == 0u);
  auto* host_reservation = host_vm::reserve(host_page * 2u);
  assert(host_reservation);
  assert((reinterpret_cast<std::uintptr_t>(host_reservation) % host_page) == 0u);
  assert(host_vm::commit(host_reservation, host_page,
                         host_vm::Protection::ReadWrite));
  auto* host_bytes = static_cast<std::byte*>(host_reservation);
  host_bytes[0] = std::byte{0x5A};
  host_bytes[host_page - 1u] = std::byte{0xA5};
  assert(host_vm::protect(host_reservation, host_page,
                          host_vm::Protection::Read));
  assert(host_bytes[0] == std::byte{0x5A});
  assert(host_vm::protect(host_reservation, host_page,
                          host_vm::Protection::ReadWrite));
  assert(host_vm::decommit(host_reservation, host_page));
  assert(host_vm::commit(host_reservation, host_page,
                         host_vm::Protection::ReadWrite));
  assert(host_bytes[0] == std::byte{} &&
         host_bytes[host_page - 1u] == std::byte{});
  host_bytes[0] = std::byte{0x7C};
  assert(host_vm::discard(host_reservation, host_page,
                          host_vm::Protection::ReadWrite));
  assert(host_bytes[0] == std::byte{});
  host_vm::release(host_reservation, host_page * 2u);

  AddressSpace mem;
  assert(mem.initialize());
  // Auto currently selects the compact table on the qualified Linux x86-64
  // baseline because the Phase 6 benchmark measured it faster than the direct
  // aperture. Direct mode remains independently testable and build-selectable.
#if defined(XENON_MEMORY_DEFAULT_DIRECT_APERTURE) && XENON_MEMORY_DEFAULT_DIRECT_APERTURE
  if (host_vm::supports_fixed_shared_mapping() && sizeof(void*) >= 8u) {
    assert(mem.direct_aperture_active());
  }
#else
  assert(!mem.direct_aperture_active());
#endif

  auto rs = AddressSpace::regions();
  assert(rs.size() == 9);
  assert(rs[0].base == 0x00000000u && rs[0].allocation_page_size == 0x1000u);
  assert(rs[1].base == 0x40000000u && rs[1].allocation_page_size == 0x10000u);
  assert(rs[5].base == 0xA0000000u && rs[5].kind == RegionKind::PhysicalAlias);

  assert(faults([&] { (void)mem.read8(0); }, FaultReason::Uncommitted));

  // 4 KiB virtual mapping backed by real 512 MiB physical RAM.
  assert(mem.commit_fixed(0x00100000u, 0x2000u, kReadWrite));
  if (mem.direct_aperture_active()) {
    assert(mem.direct_aperture_maps(0x00100000u));
    assert(mem.direct_aperture_maps(0x00101000u));
  }

  // Phase 6 must remain optional. Force the portable compact-translation mode
  // and verify that Xbox-visible behavior is identical when no aperture is
  // requested.
  {
    AddressSpace compact_only(GuestTranslationMode::Compact);
    assert(compact_only.initialize());
    assert(!compact_only.direct_aperture_active());
    assert(compact_only.commit_fixed(0x00100000u, 0x1000u, kReadWrite));
    assert(!compact_only.direct_aperture_maps(0x00100000u));
    compact_only.write32_be(0x00100000u, 0x13579BDFu);
    const auto compact_phys = compact_only.get_physical_address(0x00100000u);
    assert(compact_phys != 0xFFFFFFFFu);
    assert(compact_only.read32_be(kPhysical64KBase + compact_phys) ==
           0x13579BDFu);
  }

  // Dynamic aperture mappings follow commit/decommit/recommit and XEX aliases
  // while preserving one physical backing object.
  if (host_vm::supports_fixed_shared_mapping() && sizeof(void*) >= 8u) {
    AddressSpace aperture_mem(GuestTranslationMode::DirectAperture);
    assert(aperture_mem.initialize());
    assert(aperture_mem.direct_aperture_active());
    assert(aperture_mem.direct_aperture_maps(kGpuWritebackBase));
    assert(aperture_mem.direct_aperture_maps(kPhysical64KBase));
    assert(aperture_mem.direct_aperture_maps(kPhysical16MBase));
    assert(aperture_mem.direct_aperture_maps(kPhysical4KBase));
    constexpr GuestAddress kAperturePage = 0x00200000u;
    assert(aperture_mem.commit_fixed(kAperturePage, kBasePageSize, kReadWrite));
    assert(aperture_mem.direct_aperture_maps(kAperturePage));
    aperture_mem.write32_be(kAperturePage, 0x2468ACE0u);
    const auto aperture_phys = aperture_mem.get_physical_address(kAperturePage);
    assert(aperture_mem.read32_be(kPhysical64KBase + aperture_phys) ==
           0x2468ACE0u);
    assert(aperture_mem.decommit(kAperturePage, kBasePageSize));
    assert(!aperture_mem.direct_aperture_maps(kAperturePage));
    assert(aperture_mem.commit_fixed(kAperturePage, kBasePageSize, kReadWrite));
    assert(aperture_mem.direct_aperture_maps(kAperturePage));

    constexpr GuestAddress kXexPage = 0x82000000u;
    constexpr GuestAddress kXexAlias = 0x92000000u;
    assert(aperture_mem.commit_fixed(kXexPage, kLargePageSize, kReadWrite));
    assert(aperture_mem.direct_aperture_maps(kXexPage));
    assert(aperture_mem.direct_aperture_maps(kXexAlias));
    aperture_mem.write32_be(kXexPage, 0xA5C31F70u);
    assert(aperture_mem.read32_be(kXexAlias) == 0xA5C31F70u);

    // Direct mappings obey the same read-side lifetime rule as physical-page
    // reclamation. If a fast context could have observed the old direct entry,
    // decommit first removes kDirectAperture but leaves the host alias intact
    // until that context is gone. Recommit can then safely establish the new
    // mapping.
    constexpr GuestAddress kQuiescentPage = 0x00300000u;
    assert(aperture_mem.commit_fixed(kQuiescentPage, kBasePageSize, kReadWrite));
    aperture_mem.write32_be(kQuiescentPage, 0xDEADBEEFu);
    {
      auto held = aperture_mem.access_context();
      xenon::cpu::MemoryAccessContext::PhysicalResolution resolution{};
      assert(held.resolve_physical_ram(kQuiescentPage, sizeof(std::uint32_t),
                                       false, alignof(std::uint32_t),
                                       resolution));
      assert(resolution.ptr !=
             aperture_mem.physical_data(resolution.physical_address));
      assert(aperture_mem.decommit(kQuiescentPage, kBasePageSize));
      assert(!aperture_mem.direct_aperture_maps(kQuiescentPage));
      // The old host alias remains mapped during the read-side grace period.
      assert(resolution.ptr != nullptr);
    }
    assert(aperture_mem.commit_fixed(kQuiescentPage, kBasePageSize, kReadWrite));
    assert(aperture_mem.direct_aperture_maps(kQuiescentPage));
  }

  // Executable mappings use physical-page generations rather than arbitrary
  // callbacks on every scalar store. CPU writes, controlled external writes
  // and icbi all make an existing native translation observably stale.
  assert(mem.protect(0x00100000u, kBasePageSize, kReadWriteExecute));
  const auto executable_physical = mem.get_physical_address(0x00100000u);
  const auto executable_generation = mem.executable_generation(0x00100000u);
  assert(executable_generation != 0u);
  mem.write32_be(0x00100000u, 0x60000000u);
  const auto after_cpu_code_write = mem.executable_generation(0x00100000u);
  assert(after_cpu_code_write != executable_generation);
  const std::array external_code{std::byte{0x4E}, std::byte{0x80},
                                 std::byte{0x00}, std::byte{0x20}};
  assert(mem.write_physical(executable_physical, external_code));
  const auto after_external_code_write =
      mem.executable_generation(0x00100000u);
  assert(after_external_code_write != after_cpu_code_write);
  mem.instruction_cache_invalidate(0x00100000u);
  assert(mem.executable_generation(0x00100000u) !=
         after_external_code_write);
  assert(mem.protect(0x00100000u, kBasePageSize, kReadWrite));
  assert(mem.executable_generation(0x00100000u) == 0u);

  // Memory V2 Phase 10: canonical PPC ordering is represented explicitly,
  // rather than mapping every barrier to seq_cst. lwsync orders LL/LS/SS but
  // deliberately does not order Store->Load; eieio applies to device-like
  // domains rather than ordinary cached RAM; isync is an instruction boundary.
  using xenon::cpu::BarrierKind;
  using xenon::cpu::MemoryOrderingDomain;
  using xenon::cpu::OrderedAccess;
  static_assert(xenon::cpu::barrier_orders(BarrierKind::Sync,
                                           OrderedAccess::Store,
                                           OrderedAccess::Load));
  static_assert(xenon::cpu::barrier_orders(BarrierKind::LightweightSync,
                                           OrderedAccess::Load,
                                           OrderedAccess::Load));
  static_assert(xenon::cpu::barrier_orders(BarrierKind::LightweightSync,
                                           OrderedAccess::Load,
                                           OrderedAccess::Store));
  static_assert(xenon::cpu::barrier_orders(BarrierKind::LightweightSync,
                                           OrderedAccess::Store,
                                           OrderedAccess::Store));
  static_assert(!xenon::cpu::barrier_orders(BarrierKind::LightweightSync,
                                            OrderedAccess::Store,
                                            OrderedAccess::Load));
  static_assert(!xenon::cpu::barrier_orders(BarrierKind::Eieio,
                                            OrderedAccess::Store,
                                            OrderedAccess::Store,
                                            MemoryOrderingDomain::Normal));
  static_assert(xenon::cpu::barrier_orders(BarrierKind::Eieio,
                                           OrderedAccess::Store,
                                           OrderedAccess::Store,
                                           MemoryOrderingDomain::Device));
  static_assert(xenon::cpu::barrier_orders(BarrierKind::Eieio,
                                           OrderedAccess::Load,
                                           OrderedAccess::Store,
                                           MemoryOrderingDomain::WriteCombined));
  static_assert(xenon::cpu::barrier_orders(BarrierKind::Eieio,
                                           OrderedAccess::Store,
                                           OrderedAccess::Load,
                                           MemoryOrderingDomain::CacheInhibited));
  static_assert(xenon::cpu::barrier_semantics(BarrierKind::InstructionSync)
                    .instruction_sync);

  // Ordering domains come from production mapping state, not from a parallel
  // CPU-side guess. The compact hot entry distinguishes normal, WC and CI RAM;
  // MMIO remains a cold/device classification because it is already slow-path.
  {
    AddressSpace ordering_mem;
    assert(ordering_mem.initialize());
    constexpr GuestAddress normal_page = 0x00300000u;
    constexpr GuestAddress ci_page = 0x00301000u;
    constexpr GuestAddress wc_page = 0x00302000u;
    constexpr GuestAddress mmio_page = 0xFFD00000u;
    assert(ordering_mem.commit_fixed(normal_page, kBasePageSize, kReadWrite));
    assert(ordering_mem.commit_fixed(
        ci_page, kBasePageSize, kReadWrite | Protect::NoCache));
    assert(ordering_mem.commit_fixed(
        wc_page, kBasePageSize, kReadWrite | Protect::WriteCombine));
    assert(ordering_mem.add_mmio_range(
        mmio_page, kBasePageSize,
        [](GuestAddress, std::uint32_t) -> std::uint64_t { return 0u; },
        [](GuestAddress, std::uint32_t, std::uint64_t) {}, "ordering-mmio"));
    auto ordering_access = ordering_mem.access_context();
    assert(ordering_mem.ordering_domain(normal_page) ==
           MemoryOrderingDomain::Normal);
    assert(ordering_access.ordering_domain(normal_page) ==
           MemoryOrderingDomain::Normal);
    assert(ordering_mem.ordering_domain(ci_page) ==
           MemoryOrderingDomain::CacheInhibited);
    assert(ordering_access.ordering_domain(ci_page) ==
           MemoryOrderingDomain::CacheInhibited);
    assert(ordering_mem.ordering_domain(wc_page) ==
           MemoryOrderingDomain::WriteCombined);
    assert(ordering_access.ordering_domain(wc_page) ==
           MemoryOrderingDomain::WriteCombined);
    assert(ordering_mem.ordering_domain(mmio_page) ==
           MemoryOrderingDomain::Device);
    assert(ordering_access.ordering_domain(mmio_page) ==
           MemoryOrderingDomain::Device);
    // eieio is architecturally meaningful for the latter three domains, but
    // not ordinary cached RAM. The host fence may conservatively be stronger.
    assert(!xenon::cpu::barrier_orders(
        BarrierKind::Eieio, OrderedAccess::Store, OrderedAccess::Load,
        ordering_access.ordering_domain(normal_page)));
    for (const auto address : {ci_page, wc_page, mmio_page}) {
      assert(xenon::cpu::barrier_orders(
          BarrierKind::Eieio, OrderedAccess::Store, OrderedAccess::Load,
          ordering_access.ordering_domain(address)));
    }
  }

  // Message-passing litmus using actual Memory V2 relaxed RAM operations. The
  // producer's Store->Store and consumer's Load->Load ordering are both pairs
  // guaranteed by lwsync. The acknowledgement prevents a later iteration from
  // racing ahead and hiding an ordering failure.
  constexpr GuestAddress order_data = 0x00100600u;
  constexpr GuestAddress order_flag = 0x00100604u;
  constexpr GuestAddress order_ack = 0x00100608u;
  constexpr std::uint32_t order_rounds = 2000u;
  mem.write32_be(order_data, 0u);
  mem.write32_be(order_flag, 0u);
  mem.write32_be(order_ack, 0u);
  std::atomic<bool> ordering_failed{false};
  std::thread order_producer([&] {
    auto access = mem.access_context();
    for (std::uint32_t i = 1; i <= order_rounds; ++i) {
      while (access.read32_be(order_ack) != i - 1u) std::this_thread::yield();
      access.write32_be(order_data, i);
      mem.barrier(BarrierKind::LightweightSync);
      access.write32_be(order_flag, i);
    }
  });
  std::thread order_consumer([&] {
    auto access = mem.access_context();
    for (std::uint32_t i = 1; i <= order_rounds; ++i) {
      while (access.read32_be(order_flag) != i) std::this_thread::yield();
      mem.barrier(BarrierKind::LightweightSync);
      if (access.read32_be(order_data) != i) ordering_failed.store(true);
      mem.barrier(BarrierKind::LightweightSync);
      access.write32_be(order_ack, i);
    }
  });
  order_producer.join();
  order_consumer.join();
  assert(!ordering_failed.load());

  // Store-buffering litmus for heavyweight sync. PPC sync orders Store->Load,
  // so both threads observing the pre-store zero value is forbidden.
  constexpr GuestAddress sync_x = 0x00100620u;
  constexpr GuestAddress sync_y = 0x00100624u;
  for (std::uint32_t round = 0; round < 256u; ++round) {
    mem.write32_be(sync_x, 0u);
    mem.write32_be(sync_y, 0u);
    std::uint32_t r0 = 0u;
    std::uint32_t r1 = 0u;
    std::thread left([&] {
      auto access = mem.access_context();
      access.write32_be(sync_x, 1u);
      mem.barrier(BarrierKind::Sync);
      r0 = access.read32_be(sync_y);
    });
    std::thread right([&] {
      auto access = mem.access_context();
      access.write32_be(sync_y, 1u);
      mem.barrier(BarrierKind::Sync);
      r1 = access.read32_be(sync_x);
    });
    left.join();
    right.join();
    assert(r0 != 0u || r1 != 0u);
  }
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

  // Phase 4 completion stress: use the public AddressSpace MemoryPort surface,
  // not a pre-acquired fast context, while cold mapping management and a
  // controlled external/DMA writer run concurrently. Stable ordinary RAM must
  // remain independent of the global management mutex and every access to the
  // shared DMA word must stay within the two atomically published 64-bit
  // patterns. This is also exercised under TSan in the validation matrix.
  {
    AddressSpace concurrent_mem;
    assert(concurrent_mem.initialize());
    constexpr GuestAddress kStableBase = 0x00100000u;
    constexpr GuestAddress kDmaWord = 0x00100800u;
    constexpr GuestAddress kChurnPage = 0x00200000u;
    assert(concurrent_mem.commit_fixed(kStableBase, 0x4000u, kReadWrite));
    const auto dma_physical = concurrent_mem.get_physical_address(kDmaWord);
    assert(dma_physical != 0xFFFFFFFFu && (dma_physical & 7u) == 0u);

    constexpr std::uint64_t kPatternA = 0x1111111111111111ull;
    constexpr std::uint64_t kPatternB = 0x2222222222222222ull;
    const std::array<std::byte, 8> pattern_a{
        std::byte{0x11}, std::byte{0x11}, std::byte{0x11}, std::byte{0x11},
        std::byte{0x11}, std::byte{0x11}, std::byte{0x11}, std::byte{0x11}};
    const std::array<std::byte, 8> pattern_b{
        std::byte{0x22}, std::byte{0x22}, std::byte{0x22}, std::byte{0x22},
        std::byte{0x22}, std::byte{0x22}, std::byte{0x22}, std::byte{0x22}};
    assert(concurrent_mem.write_physical(dma_physical, pattern_a));

    std::atomic<bool> go{false};
    std::atomic<bool> failed{false};
    std::array<std::thread, 6> cpu_threads{};
    for (std::uint32_t thread = 0; thread < cpu_threads.size(); ++thread) {
      cpu_threads[thread] = std::thread([&, thread] {
        const GuestAddress address =
            kStableBase + 0x100u + static_cast<GuestAddress>(thread * 8u);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint32_t i = 0; i < 4000u; ++i) {
          const auto value = (std::uint64_t{thread + 1u} << 56u) | i;
          concurrent_mem.write64_be(address, value);
          if (concurrent_mem.read64_be(address) != value) {
            failed.store(true, std::memory_order_relaxed);
            return;
          }
        }
      });
    }

    std::thread dma_writer([&] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      for (std::uint32_t i = 0; i < 4000u; ++i) {
        const auto& pattern = (i & 1u) ? pattern_a : pattern_b;
        if (!concurrent_mem.write_physical(dma_physical, pattern)) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
      }
    });
    std::thread dma_reader([&] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      for (std::uint32_t i = 0; i < 4000u; ++i) {
        const auto value = concurrent_mem.read64_be(kDmaWord);
        if (value != kPatternA && value != kPatternB) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
      }
    });
    std::thread mapping_churn([&] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      for (std::uint32_t i = 0; i < 1000u; ++i) {
        if (!concurrent_mem.commit_fixed(kChurnPage, kBasePageSize, kReadWrite) ||
            !concurrent_mem.release(kChurnPage)) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
      }
    });

    go.store(true, std::memory_order_release);
    for (auto& worker : cpu_threads) worker.join();
    dma_writer.join();
    dma_reader.join();
    mapping_churn.join();
    assert(!failed.load(std::memory_order_relaxed));
    assert(concurrent_mem.validate_invariants());
  }

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

  // C-view and A-view are fixed architectural aliases of the same physical RAM.
  mem.write32_be(0xC0000100u, 0xA5A55A5Au);
  assert(mem.read32_be(0xA0000100u) == 0xA5A55A5Au);

  // E-view begins one base page into physical memory.
  mem.write32_be(0xE0000000u, 0x55667788u);
  assert(mem.read32_be(0xA0001000u) == 0x55667788u);

  // The two XEX windows are aliases of one logical image mapping.
  assert(mem.commit_fixed(0x82000000u, 0x10000u, kReadWriteExecute));
  mem.write32_be(0x82001234u, 0x0BADF00Du);
  assert(mem.fetch32_be(0x82001234u) == 0x0BADF00Du);
  assert(mem.read32_be(0x92001234u) == 0x0BADF00Du);
  assert(mem.get_physical_address(0x82001234u) == mem.get_physical_address(0x92001234u));
  const auto xex_aliases = mem.dynamic_guest_aliases_for_physical(
      mem.get_physical_address(0x82001234u));
  assert(std::find(xex_aliases.begin(), xex_aliases.end(), 0x82001234u) !=
         xex_aliases.end());
  assert(std::find(xex_aliases.begin(), xex_aliases.end(), 0x92001234u) !=
         xex_aliases.end());
  assert(mem.validate_invariants());

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

  // The V2 monitor stores only six live reservation slots plus a sticky
  // per-granule hint bitmap (~512 KiB), not one 32-bit generation counter for
  // every 128-byte granule (~16 MiB in the V1 design).
  assert(mem.reservation_monitor_storage_bytes() < 600u * 1024u);

  // LR/SC identity is physical rather than virtual. A conditional store
  // through an Xbox physical alias to exactly the reserved physical word must
  // therefore succeed when no intervening writer touched the granule.
  mem.write32_be(0x00100000u, 0x30303030u);
  const auto alias_token = mem.reserve32(0x00100000u, reserved);
  assert(alias_token != 0u && reserved == 0x30303030u);
  assert(mem.store_conditional32(physical_alias, alias_token, 0x31313131u));
  assert(mem.read32_be(0x00100000u) == 0x31313131u);

  // The same monitor handles 64-bit reservations.
  mem.write64_be(0x00100100u, 0x1122334455667788ull);
  std::uint64_t reserved64{};
  const auto token64 = mem.reserve64(0x00100100u, reserved64);
  assert(token64 != 0u && reserved64 == 0x1122334455667788ull);
  const auto physical64 = mem.get_physical_address(0x00100100u);
  assert(physical64 != 0xFFFFFFFFu);
  assert(mem.store_conditional64(0xA0000000u + physical64, token64,
                                 0x8877665544332211ull));
  assert(mem.read64_be(0x00100100u) == 0x8877665544332211ull);

  // Six simultaneously live reservations fit exactly in the Xenon monitor.
  // Keep them on separate 128-byte granules so the conditional stores do not
  // intentionally invalidate one another.
  {
    constexpr std::size_t kHardwareThreads = 6;
    constexpr GuestAddress kBase = 0x00100400u;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::array<std::atomic<bool>, kHardwareThreads> succeeded{};
    std::array<std::thread, kHardwareThreads> workers;
    for (std::size_t i = 0; i < kHardwareThreads; ++i) {
      const auto address = kBase + static_cast<GuestAddress>(i * 0x100u);
      mem.write32_be(address, static_cast<std::uint32_t>(0x100u + i));
    }
    for (std::size_t i = 0; i < kHardwareThreads; ++i) {
      const auto address = kBase + static_cast<GuestAddress>(i * 0x100u);
      workers[i] = std::thread([&, i, address] {
        std::uint32_t observed{};
        const auto t = mem.reserve32(address, observed);
        assert(t != 0u);
        ready.fetch_add(1u, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        succeeded[i].store(
            mem.store_conditional32(address, t, observed + 1u),
            std::memory_order_release);
      });
    }
    while (ready.load(std::memory_order_acquire) != kHardwareThreads)
      std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    for (const auto& ok : succeeded) assert(ok.load(std::memory_order_acquire));
  }

  // Heavy six-thread contention on one word must not lose successful updates.
  // Conflicting conditional stores are expected to fail and retry.
  {
    constexpr unsigned kHardwareThreads = 6;
    constexpr unsigned kIterations = 200;
    constexpr GuestAddress kCounter = 0x00100C00u;
    mem.write32_be(kCounter, 0u);
    std::array<std::thread, kHardwareThreads> workers;
    for (auto& worker : workers) {
      worker = std::thread([&] {
        for (unsigned iteration = 0; iteration < kIterations; ++iteration) {
          for (;;) {
            std::uint32_t observed{};
            const auto t = mem.reserve32(kCounter, observed);
            if (!t) {
              std::this_thread::yield();
              continue;
            }
            if (mem.store_conditional32(kCounter, t, observed + 1u)) break;
          }
        }
      });
    }
    for (auto& worker : workers) worker.join();
    assert(mem.read32_be(kCounter) == kHardwareThreads * kIterations);
  }

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

  // Memory V2 reverse mappings: overlapping virtual views keep physical
  // ownership live independently and can be enumerated without scanning the
  // million-entry guest page table.
  {
    AddressSpace reverse_mem;
    assert(reverse_mem.initialize());
    constexpr GuestAddress view_a = 0x00100000u;
    constexpr GuestAddress view_b = 0x00200000u;
    std::uint32_t physical{};
    assert(reverse_mem.allocate_physical(3u * kBasePageSize, kBasePageSize,
                                         false, physical));
    assert(reverse_mem.map_virtual_to_physical(view_a, physical,
                                               3u * kBasePageSize,
                                               kReadWrite));
    assert(reverse_mem.map_virtual_to_physical(view_b,
                                               physical + kBasePageSize,
                                               2u * kBasePageSize,
                                               kReadWrite));
    const auto aliases = reverse_mem.dynamic_guest_aliases_for_physical(
        physical + kBasePageSize + 0x80u);
    assert(aliases.size() == 2u);
    assert(std::find(aliases.begin(), aliases.end(), view_a + kBasePageSize + 0x80u) !=
           aliases.end());
    assert(std::find(aliases.begin(), aliases.end(), view_b + 0x80u) !=
           aliases.end());
    reverse_mem.write32_be(view_a + kBasePageSize, 0x13579BDFu);
    assert(reverse_mem.read32_be(view_b) == 0x13579BDFu);
    assert(reverse_mem.validate_invariants());

    assert(reverse_mem.release(view_a));
    assert(!reverse_mem.free_physical(physical, 3u * kBasePageSize));
    const auto remaining_aliases =
        reverse_mem.dynamic_guest_aliases_for_physical(physical + kBasePageSize);
    assert(remaining_aliases.size() == 1u && remaining_aliases[0] == view_b);
    assert(reverse_mem.validate_invariants());
    assert(reverse_mem.release(view_b));
    assert(reverse_mem.free_physical(physical, 3u * kBasePageSize));
    assert(reverse_mem.validate_invariants());
  }

  // Decommitting an externally mapped view must clear mapping-specific state.
  // Recommitting that reservation creates normal anonymous backing which must
  // later be released normally rather than leaking as if it were still owned
  // by the old explicit physical allocation.
  {
    AddressSpace decommit_mem;
    assert(decommit_mem.initialize());
    constexpr GuestAddress va = 0x00100000u;
    std::uint32_t external_physical{};
    assert(decommit_mem.allocate_physical(kBasePageSize, kBasePageSize, false,
                                          external_physical));
    assert(decommit_mem.map_virtual_to_physical(va, external_physical,
                                                kBasePageSize, kReadWrite));
    assert(decommit_mem.decommit(va, kBasePageSize));
    assert(decommit_mem.free_physical(external_physical, kBasePageSize));
    assert(decommit_mem.commit_fixed(va, kBasePageSize, kReadWrite));
    const auto anonymous_physical = decommit_mem.get_physical_address(va);
    assert(anonymous_physical != 0xFFFFFFFFu);
    assert(decommit_mem.release(va));
    assert(decommit_mem.validate_invariants());
    std::uint32_t reusable{};
    assert(decommit_mem.allocate_physical(kBasePageSize, kBasePageSize, false,
                                          reusable));
    assert(reusable == anonymous_physical);
    assert(decommit_mem.free_physical(reusable, kBasePageSize));
  }

  // Randomized alias model: repeatedly map and unmap a fixed physical pool
  // through independent virtual views. The reverse map must agree with the
  // model and all aliases of a physical page must observe the same bytes.
  {
    AddressSpace model_mem;
    assert(model_mem.initialize());
    constexpr std::uint32_t physical_pages = 16u;
    constexpr std::uint32_t virtual_slots = 32u;
    constexpr GuestAddress virtual_base = 0x01000000u;
    std::uint32_t pool{};
    assert(model_mem.allocate_physical(physical_pages * kBasePageSize,
                                       kBasePageSize, false, pool));
    std::array<int, virtual_slots> slot_physical{};
    slot_physical.fill(-1);
    std::uint32_t rng = 0xC001D00Du;
    auto next_random = [&]() {
      rng = rng * 1664525u + 1013904223u;
      return rng;
    };

    for (std::uint32_t iteration = 0; iteration < 2000u; ++iteration) {
      const auto slot = next_random() % virtual_slots;
      const auto va = virtual_base + slot * kBasePageSize;
      if (slot_physical[slot] >= 0 && (next_random() & 1u)) {
        assert(model_mem.release(va));
        slot_physical[slot] = -1;
      } else if (slot_physical[slot] < 0) {
        const auto physical_index = next_random() % physical_pages;
        assert(model_mem.map_virtual_to_physical(
            va, pool + physical_index * kBasePageSize, kBasePageSize,
            kReadWrite));
        slot_physical[slot] = static_cast<int>(physical_index);
      }

      if ((iteration % 40u) == 0u) {
        for (std::uint32_t physical_index = 0; physical_index < physical_pages;
             ++physical_index) {
          std::vector<GuestAddress> expected;
          for (std::uint32_t candidate = 0; candidate < virtual_slots;
               ++candidate) {
            if (slot_physical[candidate] == static_cast<int>(physical_index)) {
              expected.push_back(virtual_base + candidate * kBasePageSize + 0x24u);
            }
          }
          std::sort(expected.begin(), expected.end());
          const auto actual = model_mem.dynamic_guest_aliases_for_physical(
              pool + physical_index * kBasePageSize + 0x24u);
          assert(actual == expected);
          if (!expected.empty()) {
            const auto value = 0x60000000u | (iteration << 4u) | physical_index;
            model_mem.write32_be(expected.front(), value);
            for (const auto alias : expected) {
              assert(model_mem.read32_be(alias) == value);
            }
          }
        }
      }
      if ((iteration % 100u) == 0u) assert(model_mem.validate_invariants());
    }

    for (std::uint32_t slot = 0; slot < virtual_slots; ++slot) {
      if (slot_physical[slot] >= 0) {
        assert(model_mem.release(virtual_base + slot * kBasePageSize));
      }
    }
    assert(model_mem.validate_invariants());
    assert(model_mem.free_physical(pool, physical_pages * kBasePageSize));
    assert(model_mem.validate_invariants());
  }

  // Memory V2 read-side quiescence: a page unpublished by release may not be
  // recycled while a pre-existing fast access context is still alive. This
  // prevents an old hot-page translation from becoming an ABA reference to a
  // different allocation that reused the same physical frame.
  {
    AddressSpace quiescence_mem;
    assert(quiescence_mem.initialize());
    constexpr GuestAddress old_va = 0x00100000u;
    assert(quiescence_mem.commit_fixed(old_va, kBasePageSize, kReadWrite));
    quiescence_mem.write32_be(old_va, 0xCAFEBABEu);
    const auto retired_phys = quiescence_mem.get_physical_address(old_va);
    assert(retired_phys != 0xFFFFFFFFu);

    {
      auto old_context = quiescence_mem.access_context();
      assert(old_context.has_fast_path());
      assert(old_context.read32_be(old_va) == 0xCAFEBABEu);
      assert(quiescence_mem.release(old_va));

      std::uint32_t while_reader_alive{};
      assert(quiescence_mem.allocate_physical(
          kBasePageSize, kBasePageSize, false, while_reader_alive));
      assert(while_reader_alive != retired_phys);
      assert(quiescence_mem.free_physical(while_reader_alive, kBasePageSize));
    }

    std::uint32_t after_quiescence{};
    assert(quiescence_mem.allocate_physical(
        kBasePageSize, kBasePageSize, false, after_quiescence));
    assert(after_quiescence == retired_phys);
    assert(quiescence_mem.free_physical(after_quiescence, kBasePageSize));
  }

  // Memory V2 physical range allocator: bottom-up and top-down allocation
  // preserve Xbox-facing alignment semantics without scanning every physical
  // page. The first 16 MiB remain reserved for the GPU writeback/XPS window.
  {
    AddressSpace allocator_mem;
    assert(allocator_mem.initialize());

    std::uint32_t low{};
    assert(allocator_mem.allocate_physical(kBasePageSize, kBasePageSize,
                                           false, low));
    assert(low == kHugePageSize);

    std::uint32_t high{};
    assert(allocator_mem.allocate_physical(kBasePageSize, kBasePageSize,
                                           true, high));
    assert(high == kPhysicalMemorySize - kBasePageSize);

    std::uint32_t aligned{};
    constexpr std::uint32_t kAlignment = 0x40000u;
    assert(allocator_mem.allocate_physical(3u * kBasePageSize, kAlignment,
                                           false, aligned));
    assert((aligned & (kAlignment - 1u)) == 0u);

    assert(allocator_mem.free_physical(low, kBasePageSize));
    assert(allocator_mem.free_physical(high, kBasePageSize));
    assert(allocator_mem.free_physical(aligned, 3u * kBasePageSize));
  }

  // Mapping previously unowned physical RAM claims those frames in the same
  // allocator index used by explicit allocations. The allocator must skip a
  // live mapped frame and may reuse it only after the mapping and physical
  // ownership are both released.
  {
    AddressSpace claimed_mem;
    assert(claimed_mem.initialize());
    constexpr GuestAddress claim_va = 0x00300000u;
    constexpr std::uint32_t claim_phys = kHugePageSize;
    assert(claimed_mem.map_virtual_to_physical(
        claim_va, claim_phys, kBasePageSize, kReadWrite));

    std::uint32_t while_mapped{};
    assert(claimed_mem.allocate_physical(kBasePageSize, kBasePageSize,
                                         false, while_mapped));
    assert(while_mapped == claim_phys + kBasePageSize);
    assert(claimed_mem.free_physical(while_mapped, kBasePageSize));

    assert(claimed_mem.release(claim_va));
    assert(claimed_mem.free_physical(claim_phys, kBasePageSize));
    std::uint32_t after_release{};
    assert(claimed_mem.allocate_physical(kBasePageSize, kBasePageSize,
                                         false, after_release));
    assert(after_release == claim_phys);
    assert(claimed_mem.free_physical(after_release, kBasePageSize));
  }

  // Adjacent released runs coalesce. A later larger allocation must be able to
  // reuse the combined hole rather than being defeated by fragmentation.
  {
    AddressSpace coalesce_mem;
    assert(coalesce_mem.initialize());
    constexpr std::uint32_t kRunSize = 8u * kBasePageSize;
    std::uint32_t a{}, b{}, c{};
    assert(coalesce_mem.allocate_physical(kRunSize, kBasePageSize, false, a));
    assert(coalesce_mem.allocate_physical(kRunSize, kBasePageSize, false, b));
    assert(coalesce_mem.allocate_physical(kRunSize, kBasePageSize, false, c));
    assert(b == a + kRunSize);
    assert(c == b + kRunSize);
    assert(coalesce_mem.free_physical(b, kRunSize));
    assert(coalesce_mem.free_physical(a, kRunSize));

    std::uint32_t combined{};
    assert(coalesce_mem.allocate_physical(2u * kRunSize, kBasePageSize,
                                           false, combined));
    assert(combined == a);
    assert(coalesce_mem.free_physical(combined, 2u * kRunSize));
    assert(coalesce_mem.free_physical(c, kRunSize));
  }

  // Deterministic fragmentation stress: repeatedly allocate/free differently
  // sized and aligned runs in both directions, checking that no live physical
  // allocations overlap. Freeing everything must reconstruct one coalesced
  // bottom-up range beginning immediately after the 16 MiB system reservation.
  {
    AddressSpace fragmented_mem;
    assert(fragmented_mem.initialize());
    struct Allocation {
      std::uint32_t base{};
      std::uint32_t size{};
    };
    std::vector<Allocation> live;
    std::uint32_t rng = 0x6D2B79F5u;
    auto next_random = [&]() {
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      return rng;
    };
    auto overlaps_live = [&](std::uint32_t base, std::uint32_t size) {
      const auto end = std::uint64_t(base) + size;
      for (const auto& allocation : live) {
        const auto other_end = std::uint64_t(allocation.base) + allocation.size;
        if (std::uint64_t(base) < other_end &&
            std::uint64_t(allocation.base) < end) {
          return true;
        }
      }
      return false;
    };

    for (std::uint32_t iteration = 0; iteration < 2000u; ++iteration) {
      const auto choice = next_random();
      if (!live.empty() && ((choice & 3u) == 0u || live.size() > 256u)) {
        const auto index = next_random() % live.size();
        const auto allocation = live[index];
        assert(fragmented_mem.free_physical(allocation.base, allocation.size));
        live[index] = live.back();
        live.pop_back();
        continue;
      }

      const auto page_count = 1u + (next_random() % 32u);
      const auto alignment_pages = 1u << (next_random() % 7u);
      const auto size = page_count * kBasePageSize;
      const auto alignment = alignment_pages * kBasePageSize;
      const bool top_down = (next_random() & 1u) != 0u;
      std::uint32_t base{};
      if (!fragmented_mem.allocate_physical(size, alignment, top_down, base)) {
        continue;
      }
      assert((base & (alignment - 1u)) == 0u);
      assert(base >= kHugePageSize);
      assert(std::uint64_t(base) + size <= kPhysicalMemorySize);
      assert(!overlaps_live(base, size));
      live.push_back({base, size});
    }

    for (const auto& allocation : live) {
      assert(fragmented_mem.free_physical(allocation.base, allocation.size));
    }
    live.clear();

    std::uint32_t coalesced{};
    constexpr std::uint32_t kLargeRun = 1024u * kBasePageSize;
    assert(fragmented_mem.allocate_physical(kLargeRun, kBasePageSize,
                                             false, coalesced));
    assert(coalesced == kHugePageSize);
    assert(fragmented_mem.free_physical(coalesced, kLargeRun));
  }

  // 64 KiB virtual allocations use the second guest heap and preserve alignment.
  GuestAddress large_alloc{};
  assert(mem.allocate(0x18000u, 0x10000u, kReadWrite, true, large_alloc, kLargePageSize));
  assert((large_alloc & 0xFFFFu) == 0 && large_alloc >= kVirtual64KBase && large_alloc <= kVirtual64KEnd);
  auto large_q = mem.query(large_alloc);
  assert(large_q && large_q->page_size == kLargePageSize && large_q->allocation_size == 0x20000u);
  assert(mem.release(large_alloc));

  // Controlled external/DMA writes automatically invalidate reservations and
  // publish Xenon-owned coherency state; callers cannot forget a separate
  // notify step after mutating raw physical backing.
  const auto token_dma = mem.reserve32(0x00100000u, reserved);
  const auto before_dma = mem.coherency().current_epoch();
  const std::array<std::byte, 1> dma_byte{std::byte{0xAA}};
  assert(mem.write_physical(phys, dma_byte));
  const auto after_dma = mem.coherency().current_epoch();
  assert(after_dma > before_dma);
  assert(mem.coherency().range_changed_since(phys, 1, before_dma, after_dma));
  std::vector<DirtyPhysicalRange> exact_dma_dirty;
  assert(mem.coherency().collect_exact_dirty_ranges(
      before_dma, after_dma, exact_dma_dirty));
  assert(exact_dma_dirty.size() == 1u);
  assert(exact_dma_dirty[0].address == phys);
  assert(exact_dma_dirty[0].size == 1u);

  // A consumer that falls behind the bounded exact-write journal must detect
  // wrap and retain the durable page-epoch fallback rather than losing writes.
  GuestMemoryCoherency wrapped_coherency;
  for (std::uint32_t i = 0;
       i <= GuestMemoryCoherency::kWriteJournalCapacity; ++i) {
    wrapped_coherency.mark_write((i & 15u) * kBasePageSize, 1u);
  }
  const auto wrapped_epoch = wrapped_coherency.current_epoch();
  std::vector<DirtyPhysicalRange> wrapped_exact;
  assert(!wrapped_coherency.collect_exact_dirty_ranges(
      0u, wrapped_epoch, wrapped_exact));
  std::vector<DirtyPhysicalRange> wrapped_pages;
  wrapped_coherency.collect_dirty_ranges(0u, wrapped_epoch, 0u,
                                         wrapped_pages);
  assert(!wrapped_pages.empty());
  std::array<std::byte, 4> snapshot{};
  assert(mem.copy_physical_range(phys, snapshot));
  assert(snapshot[0] == std::byte{0xAA});
  assert(!mem.copy_physical_range(kPhysicalMemorySize - 1u, snapshot));
  assert(!mem.store_conditional32(0x00100000u, token_dma, 0xAAAAAAAAu));

  // Scoped write spans publish their entire declared range automatically on
  // destruction. This is the escape hatch for native subsystems that need a
  // direct mutable span for a bounded operation such as a GPU resolve.
  const auto scoped_token = mem.reserve32(0x00100000u, reserved);
  const auto before_scoped_write = mem.coherency().current_epoch();
  {
    auto write = mem.physical_write_span(phys, 4);
    assert(write);
    const std::array<std::byte, 1> update{std::byte{0xBB}};
    assert(write.write(1u, update));
    assert(!write.write(4u, update));
  }
  const auto after_scoped_write = mem.coherency().current_epoch();
  assert(after_scoped_write > before_scoped_write);
  assert(mem.coherency().range_changed_since(phys, 4, before_scoped_write,
                                             after_scoped_write));
  assert(!mem.store_conditional32(0x00100000u, scoped_token, 0xBBBBBBBBu));
  assert(!mem.physical_write_span(kPhysicalMemorySize - 1u, 2u));
  assert(!mem.write_physical(kPhysicalMemorySize - 1u, snapshot));

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
  const auto split_epoch = mem.coherency().current_epoch();
  mem.write32_be(split_virtual + kBasePageSize - 2u, 0x12345678u);
  const auto split_write_epoch = mem.coherency().current_epoch();
  std::vector<DirtyPhysicalRange> exact_split_dirty;
  assert(mem.coherency().collect_exact_dirty_ranges(
      split_epoch, split_write_epoch, exact_split_dirty));
  std::uint32_t split_a_bytes{};
  std::uint32_t split_b_bytes{};
  for (const auto& range : exact_split_dirty) {
    if (range.address >= split_physical_a + kBasePageSize - 2u &&
        std::uint64_t{range.address} + range.size <=
            std::uint64_t{split_physical_a} + kBasePageSize) {
      split_a_bytes += range.size;
    } else {
      assert(range.address >= split_physical_b);
      assert(std::uint64_t{range.address} + range.size <=
             std::uint64_t{split_physical_b} + 2u);
      split_b_bytes += range.size;
    }
  }
  assert(split_a_bytes == 2u);
  assert(split_b_bytes == 2u);
  assert(mem.coherency().range_changed_since(
      split_physical_a + kBasePageSize - 2u, 2u, split_epoch,
      split_write_epoch));
  assert(mem.coherency().range_changed_since(split_physical_b, 2u,
                                             split_epoch,
                                             split_write_epoch));
  const auto split_fill_epoch = split_write_epoch;
  mem.fill(split_virtual + kBasePageSize - 2u, 4u, 0xA5u);
  const auto after_split_fill = mem.coherency().current_epoch();
  assert(mem.coherency().range_changed_since(
      split_physical_a + kBasePageSize - 2u, 2u, split_fill_epoch,
      after_split_fill));
  assert(mem.coherency().range_changed_since(split_physical_b, 2u,
                                             split_fill_epoch,
                                             after_split_fill));

  // MemoryAccessContext range operations split only at guest-page boundaries.
  // A 16-byte write crossing these discontiguous physical pages publishes two
  // physical ranges rather than sixteen scalar-write epochs.
  std::array<std::byte, 16> range_payload{};
  for (std::size_t i = 0; i < range_payload.size(); ++i) {
    range_payload[i] = static_cast<std::byte>(0x40u + i);
  }
  const auto range_epoch = mem.coherency().current_epoch();
  {
    auto access = mem.access_context();
    access.write_bytes(split_virtual + kBasePageSize - 6u, range_payload);
  }
  const auto after_range_write = mem.coherency().current_epoch();
  assert(after_range_write == range_epoch + 2u);
  std::array<std::byte, 16> range_roundtrip{};
  {
    auto access = mem.access_context();
    access.read_bytes(split_virtual + kBasePageSize - 6u, range_roundtrip);
  }
  assert(range_roundtrip == range_payload);

  assert(mem.release(split_virtual));
  assert(mem.release(split_virtual + kBasePageSize));
  assert(mem.free_physical(split_physical_a, kBasePageSize));
  assert(mem.free_physical(split_physical_b, kBasePageSize));

  // Phase 11 block/range operations: linear physical RAM uses a native
  // memmove path, including overlapping guest ranges.
  {
    AddressSpace block_mem;
    assert(block_mem.initialize());
    constexpr GuestAddress base = 0x00100000u;
    constexpr std::uint32_t bytes = 4u * kBasePageSize;
    std::uint32_t physical{};
    assert(block_mem.allocate_physical(bytes, kBasePageSize, false, physical));
    assert(block_mem.map_virtual_to_physical(base, physical, bytes, kReadWrite));

    std::vector<std::byte> expected(bytes);
    for (std::size_t i = 0; i < expected.size(); ++i) {
      expected[i] = static_cast<std::byte>((i * 37u + 11u) & 0xFFu);
    }
    {
      auto access = block_mem.access_context();
      access.write_bytes(base, expected);
    }

    constexpr std::uint32_t shift = 257u;
    std::memmove(expected.data() + shift, expected.data(), bytes - shift);
    block_mem.move(base + shift, base, bytes - shift);
    std::vector<std::byte> observed(bytes);
    {
      auto access = block_mem.access_context();
      access.read_bytes(base, observed);
    }
    assert(observed == expected);
    assert(block_mem.release(base));
    assert(block_mem.free_physical(physical, bytes));
  }

  // Non-linear but non-overlapping physical views stream through a bounded
  // scratch buffer rather than allocating a transfer-sized temporary.
  {
    AddressSpace block_mem;
    assert(block_mem.initialize());
    constexpr GuestAddress src = 0x00100000u;
    constexpr GuestAddress dst = 0x00200000u;
    std::uint32_t physical{};
    assert(block_mem.allocate_physical(4u * kBasePageSize, kBasePageSize,
                                       false, physical));
    assert(block_mem.map_virtual_to_physical(src, physical, kBasePageSize,
                                             kReadWrite));
    assert(block_mem.map_virtual_to_physical(src + kBasePageSize,
                                             physical + 2u * kBasePageSize,
                                             kBasePageSize, kReadWrite));
    assert(block_mem.map_virtual_to_physical(dst, physical + kBasePageSize,
                                             kBasePageSize, kReadWrite));
    assert(block_mem.map_virtual_to_physical(dst + kBasePageSize,
                                             physical + 3u * kBasePageSize,
                                             kBasePageSize, kReadWrite));
    std::vector<std::byte> payload(2u * kBasePageSize);
    for (std::size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<std::byte>((i * 13u + 7u) & 0xFFu);
    }
    {
      auto access = block_mem.access_context();
      access.write_bytes(src, payload);
    }
    block_mem.copy(dst, src, static_cast<std::uint32_t>(payload.size()));
    std::vector<std::byte> observed(payload.size());
    {
      auto access = block_mem.access_context();
      access.read_bytes(dst, observed);
    }
    assert(observed == payload);
    assert(block_mem.release(src));
    assert(block_mem.release(src + kBasePageSize));
    assert(block_mem.release(dst));
    assert(block_mem.release(dst + kBasePageSize));
    assert(block_mem.free_physical(physical, 4u * kBasePageSize));
  }

  // Pathological alias overlap (destination pages reverse the same physical
  // source pages) retains snapshot/memmove semantics on the explicit slow path.
  {
    AddressSpace alias_move_mem;
    assert(alias_move_mem.initialize());
    constexpr GuestAddress src = 0x00100000u;
    constexpr GuestAddress dst = 0x00200000u;
    std::uint32_t physical{};
    assert(alias_move_mem.allocate_physical(2u * kBasePageSize, kBasePageSize,
                                             false, physical));
    assert(alias_move_mem.map_virtual_to_physical(src, physical,
                                                   kBasePageSize, kReadWrite));
    assert(alias_move_mem.map_virtual_to_physical(src + kBasePageSize,
                                                   physical + kBasePageSize,
                                                   kBasePageSize, kReadWrite));
    assert(alias_move_mem.map_virtual_to_physical(dst,
                                                   physical + kBasePageSize,
                                                   kBasePageSize, kReadWrite));
    assert(alias_move_mem.map_virtual_to_physical(dst + kBasePageSize,
                                                   physical, kBasePageSize,
                                                   kReadWrite));
    std::vector<std::byte> original(2u * kBasePageSize);
    for (std::size_t i = 0; i < original.size(); ++i) {
      original[i] = static_cast<std::byte>((i * 5u + (i >> 12u) * 91u) & 0xFFu);
    }
    {
      auto access = alias_move_mem.access_context();
      access.write_bytes(src, original);
    }
    alias_move_mem.move(dst, src, static_cast<std::uint32_t>(original.size()));
    std::vector<std::byte> observed(original.size());
    {
      auto access = alias_move_mem.access_context();
      access.read_bytes(dst, observed);
    }
    assert(observed == original);
    assert(alias_move_mem.release(src));
    assert(alias_move_mem.release(src + kBasePageSize));
    assert(alias_move_mem.release(dst));
    assert(alias_move_mem.release(dst + kBasePageSize));
    assert(alias_move_mem.free_physical(physical, 2u * kBasePageSize));
  }

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
  assert(mmio_access.ordering_domain(0x7FEA0010u) ==
         MemoryOrderingDomain::Device);
  assert(mmio_access.read32_be(0x7FEA0010u) == 0x12345678u);
  mem.barrier(BarrierKind::Eieio);
  mmio_access.write32_be(0x7FEA0010u, 0x89ABCDEFu);
  mem.barrier(BarrierKind::Eieio);
  assert(mmio_last == 0x89ABCDEFu);

  // Range operations retain byte-visible MMIO behavior on the cold path while
  // ordinary RAM continues to use page/range translation. A guest copy from
  // MMIO snapshots all device reads before writing the RAM destination.
  constexpr GuestAddress byte_mmio_base = 0x7FEB0000u;
  std::array<std::uint8_t, 16> byte_mmio{};
  for (unsigned i = 0; i < byte_mmio.size(); ++i) byte_mmio[i] = 0x30u + i;
  assert(mem.add_mmio_range(
      byte_mmio_base, 0x1000u,
      [&](GuestAddress a, std::uint32_t width) -> std::uint64_t {
        assert(width == 1u);
        return byte_mmio.at(a - byte_mmio_base);
      },
      [&](GuestAddress a, std::uint32_t width, std::uint64_t value) {
        assert(width == 1u);
        byte_mmio.at(a - byte_mmio_base) = static_cast<std::uint8_t>(value);
      },
      "byte-range-device"));
  std::array<std::byte, 6> mmio_bytes{};
  mmio_access.read_bytes(byte_mmio_base + 3u, mmio_bytes);
  for (unsigned i = 0; i < mmio_bytes.size(); ++i) {
    assert(std::to_integer<std::uint8_t>(mmio_bytes[i]) == 0x33u + i);
  }
  const std::array<std::byte, 4> mmio_write{
      std::byte{0xA1}, std::byte{0xA2}, std::byte{0xA3}, std::byte{0xA4}};
  mmio_access.write_bytes(byte_mmio_base + 5u, mmio_write);
  for (unsigned i = 0; i < mmio_write.size(); ++i) {
    assert(byte_mmio[5u + i] == 0xA1u + i);
  }
  mem.copy(0x00100100u, byte_mmio_base + 5u, 4u);
  for (unsigned i = 0; i < 4; ++i) {
    assert(mem.read8(0x00100100u + i) == 0xA1u + i);
  }

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

  // Whole-address-space reset is observable by attached native GPU mirrors
  // through the Xenon-owned coherency epoch, without synchronous callbacks.
  const auto reset_epoch = mem.coherency().current_epoch();
  mem.reset();
  const auto after_reset = mem.coherency().current_epoch();
  assert(after_reset > reset_epoch);
  assert(mem.coherency().range_changed_since(0, kPhysicalMemorySize,
                                             reset_epoch, after_reset));

  std::cout << "xenon_memory_tests: ok\n";
}
