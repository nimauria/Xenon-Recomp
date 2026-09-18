#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>

#include "xenon/cpu/memory_ordering.hpp"
#include "xenon/memory/address_space.hpp"

using xenon::cpu::BarrierKind;
using xenon::cpu::MemoryOrderingDomain;
using xenon::cpu::OrderedAccess;
using xenon::memory::AddressSpace;
using xenon::memory::GuestAddress;
using xenon::memory::Protect;
using xenon::memory::kBasePageSize;
using xenon::memory::kReadWrite;

int main() {
  static_assert(xenon::cpu::barrier_orders(
      BarrierKind::Sync, OrderedAccess::Store, OrderedAccess::Load));
  static_assert(xenon::cpu::barrier_orders(
      BarrierKind::LightweightSync, OrderedAccess::Load, OrderedAccess::Load));
  static_assert(xenon::cpu::barrier_orders(
      BarrierKind::LightweightSync, OrderedAccess::Load, OrderedAccess::Store));
  static_assert(xenon::cpu::barrier_orders(
      BarrierKind::LightweightSync, OrderedAccess::Store,
      OrderedAccess::Store));
  static_assert(!xenon::cpu::barrier_orders(
      BarrierKind::LightweightSync, OrderedAccess::Store,
      OrderedAccess::Load));
  static_assert(!xenon::cpu::barrier_orders(
      BarrierKind::Eieio, OrderedAccess::Store, OrderedAccess::Load,
      MemoryOrderingDomain::Normal));
  static_assert(xenon::cpu::barrier_orders(
      BarrierKind::Eieio, OrderedAccess::Store, OrderedAccess::Load,
      MemoryOrderingDomain::Device));
  static_assert(xenon::cpu::barrier_orders(
      BarrierKind::Eieio, OrderedAccess::Store, OrderedAccess::Load,
      MemoryOrderingDomain::WriteCombined));
  static_assert(xenon::cpu::barrier_orders(
      BarrierKind::Eieio, OrderedAccess::Store, OrderedAccess::Load,
      MemoryOrderingDomain::CacheInhibited));
  static_assert(
      xenon::cpu::barrier_semantics(BarrierKind::InstructionSync)
          .instruction_sync);

  AddressSpace memory;
  assert(memory.initialize());
  constexpr GuestAddress base = 0x00400000u;
  constexpr GuestAddress normal = base;
  constexpr GuestAddress cache_inhibited = base + kBasePageSize;
  constexpr GuestAddress write_combined = base + 2u * kBasePageSize;
  constexpr GuestAddress x = base + 0x3000u;
  constexpr GuestAddress y = base + 0x3004u;
  constexpr GuestAddress flag = base + 0x3008u;
  constexpr GuestAddress ack = base + 0x300Cu;
  constexpr GuestAddress mmio = 0xFFD10000u;

  assert(memory.commit_fixed(normal, kBasePageSize, kReadWrite));
  assert(memory.commit_fixed(cache_inhibited, kBasePageSize,
                             kReadWrite | Protect::NoCache));
  assert(memory.commit_fixed(write_combined, kBasePageSize,
                             kReadWrite | Protect::WriteCombine));
  assert(memory.commit_fixed(x, kBasePageSize, kReadWrite));
  assert(memory.add_mmio_range(
      mmio, kBasePageSize,
      [](GuestAddress, std::uint32_t) -> std::uint64_t { return 0u; },
      [](GuestAddress, std::uint32_t, std::uint64_t) {}, "ordering-test"));

  auto access = memory.access_context();
  assert(access.ordering_domain(normal) == MemoryOrderingDomain::Normal);
  assert(access.ordering_domain(cache_inhibited) ==
         MemoryOrderingDomain::CacheInhibited);
  assert(access.ordering_domain(write_combined) ==
         MemoryOrderingDomain::WriteCombined);
  assert(access.ordering_domain(mmio) == MemoryOrderingDomain::Device);

  // lwsync message passing: Store->Store on the producer and Load->Load on the
  // consumer are both ordered by the guest primitive.
  access.write32_be(x, 0u);
  access.write32_be(flag, 0u);
  access.write32_be(ack, 0u);
  std::atomic<bool> failed{false};
  constexpr std::uint32_t kRounds = 128u;
  std::thread producer([&] {
    auto local = memory.access_context();
    for (std::uint32_t i = 1; i <= kRounds; ++i) {
      while (local.read32_be(ack) != i - 1u) std::this_thread::yield();
      local.write32_be(x, i);
      memory.barrier(BarrierKind::LightweightSync);
      local.write32_be(flag, i);
    }
  });
  std::thread consumer([&] {
    auto local = memory.access_context();
    for (std::uint32_t i = 1; i <= kRounds; ++i) {
      while (local.read32_be(flag) != i) std::this_thread::yield();
      memory.barrier(BarrierKind::LightweightSync);
      if (local.read32_be(x) != i) failed.store(true, std::memory_order_relaxed);
      memory.barrier(BarrierKind::LightweightSync);
      local.write32_be(ack, i);
    }
  });
  producer.join();
  consumer.join();
  assert(!failed.load(std::memory_order_relaxed));

  // Heavyweight sync orders Store->Load. The classic store-buffering result
  // where both sides read zero is therefore forbidden.
  for (std::uint32_t round = 0; round < 32u; ++round) {
    memory.write32_be(x, 0u);
    memory.write32_be(y, 0u);
    std::uint32_t left_seen = 0u;
    std::uint32_t right_seen = 0u;
    std::thread left([&] {
      auto local = memory.access_context();
      local.write32_be(x, 1u);
      memory.barrier(BarrierKind::Sync);
      left_seen = local.read32_be(y);
    });
    std::thread right([&] {
      auto local = memory.access_context();
      local.write32_be(y, 1u);
      memory.barrier(BarrierKind::Sync);
      right_seen = local.read32_be(x);
    });
    left.join();
    right.join();
    assert(left_seen != 0u || right_seen != 0u);
  }

  // Device ordering uses the dedicated PPC I/O primitive. The MMIO callbacks
  // are synchronous; the explicit barriers model visibility to external I/O.
  memory.barrier(BarrierKind::Eieio);
  memory.write32_be(mmio, 0u);
  memory.barrier(BarrierKind::Eieio);
  memory.barrier(BarrierKind::InstructionSync);
}
