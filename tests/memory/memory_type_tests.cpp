#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "xenon/cpu/memory_ordering.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/gpu_coherency.hpp"

using namespace xenon::memory;
using xenon::cpu::MemoryOrderingDomain;

namespace {

constexpr GuestAddress kNormal = 0x00400000u;
constexpr GuestAddress kWriteCombined = 0x00401000u;
constexpr GuestAddress kCacheInhibited = 0x00402000u;
constexpr GuestAddress kExecutable = 0x00403000u;
constexpr GuestAddress kMmio = 0x00410000u;

Protect rw_with(Protect type) { return kReadWrite | type; }

}  // namespace

int main() {
  static_assert(valid_memory_type_protection(kReadWrite));
  static_assert(valid_memory_type_protection(
      kReadWrite | Protect::WriteCombine));
  static_assert(valid_memory_type_protection(kReadWrite | Protect::NoCache));
  static_assert(!valid_memory_type_protection(
      kReadWrite | Protect::NoCache | Protect::WriteCombine));

  AddressSpace memory(GuestTranslationMode::DirectAperture);
  assert(memory.initialize());

  assert(memory.commit_fixed(kNormal, kBasePageSize, kReadWrite));
  assert(memory.commit_fixed(kWriteCombined, kBasePageSize,
                             rw_with(Protect::WriteCombine)));
  assert(memory.commit_fixed(kCacheInhibited, kBasePageSize,
                             rw_with(Protect::NoCache)));
  assert(memory.commit_fixed(kExecutable, kBasePageSize,
                             kReadWriteExecute));
  assert(memory.add_mmio_range(
      kMmio, kBasePageSize,
      [](GuestAddress, std::uint32_t) -> std::uint64_t { return 0x55u; },
      [](GuestAddress, std::uint32_t, std::uint64_t) {}, "phase16-device"));

  const auto normal = memory.memory_type_info(kNormal);
  assert(normal.mapped);
  assert(normal.type == MemoryType::NormalCached);
  assert(normal.host_policy == HostMappingPolicy::DefaultCachedShared);
  assert(normal.direct_aperture_eligible);
  assert(!normal.explicit_device_visibility);

  const auto wc = memory.memory_type_info(kWriteCombined);
  assert(wc.mapped);
  assert(wc.type == MemoryType::WriteCombined);
  assert(wc.host_policy == HostMappingPolicy::TranslatedWriteCombined);
  assert(!wc.direct_aperture_eligible);
  assert(wc.explicit_device_visibility);
  assert(memory.ordering_domain(kWriteCombined) ==
         MemoryOrderingDomain::WriteCombined);

  const auto ci = memory.memory_type_info(kCacheInhibited);
  assert(ci.mapped);
  assert(ci.type == MemoryType::CacheInhibited);
  assert(ci.host_policy == HostMappingPolicy::TranslatedCacheInhibited);
  assert(!ci.direct_aperture_eligible);
  assert(ci.explicit_device_visibility);
  assert(memory.ordering_domain(kCacheInhibited) ==
         MemoryOrderingDomain::CacheInhibited);

  const auto executable = memory.memory_type_info(kExecutable);
  assert(executable.mapped && executable.executable);
  assert(executable.type == MemoryType::NormalCached);

  const auto device = memory.memory_type_info(kMmio);
  assert(device.mapped);
  assert(device.type == MemoryType::Device);
  assert(device.host_policy == HostMappingPolicy::DeviceDispatcher);
  assert(!device.direct_aperture_eligible);
  assert(device.explicit_device_visibility);
  assert(memory.ordering_domain(kMmio) == MemoryOrderingDomain::Device);

  // Query-visible protection retains the Xbox cache-type bits.
  auto wc_query = memory.query(kWriteCombined);
  assert(wc_query.has_value());
  assert(has(wc_query->current_protect, Protect::WriteCombine));
  auto ci_query = memory.query(kCacheInhibited);
  assert(ci_query.has_value());
  assert(has(ci_query->current_protect, Protect::NoCache));

  // Mutually exclusive guest cache modes are rejected consistently by all
  // management entry points that accept protection flags.
  constexpr Protect kInvalid =
      kReadWrite | Protect::NoCache | Protect::WriteCombine;
  assert(!memory.commit_fixed(0x00420000u, kBasePageSize, kInvalid));
  assert(!memory.protect(kNormal, kBasePageSize, kInvalid));
  assert(!memory.map_virtual_to_physical(0x00420000u, 0u, kBasePageSize,
                                         kInvalid));

  // Native direct aliases are only used for ordinary cached RAM. Special Xbox
  // cache types stay on compact translation so the shared physical backing is
  // never given conflicting host cache attributes through different aliases.
  if (memory.direct_aperture_active()) {
    assert(memory.direct_aperture_maps(kNormal));
    assert(!memory.direct_aperture_maps(kWriteCombined));
    assert(!memory.direct_aperture_maps(kCacheInhibited));

    Protect old = Protect::None;
    assert(memory.protect(kNormal, kBasePageSize,
                          rw_with(Protect::WriteCombine), &old));
    assert(old == kReadWrite);
    assert(!memory.direct_aperture_maps(kNormal));
    assert(memory.memory_type_info(kNormal).type == MemoryType::WriteCombined);
    assert(memory.protect(kNormal, kBasePageSize, kReadWrite));
    assert(memory.memory_type_info(kNormal).type == MemoryType::NormalCached);
    assert(memory.direct_aperture_maps(kNormal));
  }

  // CPU writes publish their source cache/order domain into the canonical
  // Xenon dirty journal, including the generated fast path.
  const auto before = memory.coherency().current_epoch();
  {
    auto access = memory.access_context();
    access.write32_be(kNormal, 0x11111111u);
    access.write32_be(kWriteCombined, 0x22222222u);
    access.write32_be(kCacheInhibited, 0x33333333u);
  }
  const auto through = memory.coherency().current_epoch();
  std::vector<DirtyPhysicalWrite> writes;
  assert(memory.coherency().collect_exact_dirty_writes(before, through,
                                                       writes));
  assert(writes.size() == 3u);
  assert(writes[0].ordering_domain == MemoryOrderingDomain::Normal);
  assert(writes[1].ordering_domain == MemoryOrderingDomain::WriteCombined);
  assert(writes[2].ordering_domain == MemoryOrderingDomain::CacheInhibited);

  const auto normal_physical = memory.get_physical_address(kNormal);
  const auto wc_physical = memory.get_physical_address(kWriteCombined);
  const auto ci_physical = memory.get_physical_address(kCacheInhibited);
  assert(normal_physical != 0xFFFFFFFFu);
  assert(wc_physical != 0xFFFFFFFFu);
  assert(ci_physical != 0xFFFFFFFFu);

  // The backend-neutral GPU planner retains the strongest source visibility
  // domain for each requested upload. Discrete mirrors still perform a copy;
  // shared/UMA backends can select cache-maintenance/barrier policy from this.
  GuestMemoryGpuCoherency gpu;
  gpu.reset(kPhysicalMemorySize, false);
  auto normal_plan = gpu.plan_upload(memory.coherency(), normal_physical, 4u);
  assert(normal_plan.source_domain == MemoryOrderingDomain::Normal);
  gpu.commit_cpu_upload(normal_physical, 4u);

  auto wc_plan = gpu.plan_upload(memory.coherency(), wc_physical, 4u);
  assert(wc_plan.source_domain == MemoryOrderingDomain::WriteCombined);
  // A rollback must not erase the WC identity.
  gpu.rollback_cpu_upload(wc_physical, 4u);
  wc_plan = gpu.plan_upload(memory.coherency(), wc_physical, 4u);
  assert(wc_plan.source_domain == MemoryOrderingDomain::WriteCombined);
  gpu.commit_cpu_upload(wc_physical, 4u);

  const auto ci_plan = gpu.plan_upload(memory.coherency(), ci_physical, 4u);
  assert(ci_plan.source_domain == MemoryOrderingDomain::CacheInhibited);

  // If the byte-exact publication journal wraps, cache-type identity cannot be
  // reconstructed safely from page epochs alone. The fallback must strengthen
  // visibility to cache-inhibited rather than silently weakening it to Normal.
  GuestMemoryCoherency overflow_publications;
  for (std::uint32_t i = 0;
       i < GuestMemoryCoherency::kWriteJournalCapacity + 1u; ++i) {
    overflow_publications.mark_write(
        0x8000u, 1u, MemoryOrderingDomain::WriteCombined);
  }
  GuestMemoryGpuCoherency overflow_gpu;
  overflow_gpu.reset(0x10000u, false);
  const auto overflow_plan =
      overflow_gpu.plan_upload(overflow_publications, 0x8000u, 1u);
  assert(!overflow_plan.exact_history);
  assert(overflow_plan.source_domain ==
         MemoryOrderingDomain::CacheInhibited);

  // Controlled physical DMA/GPU writes are explicitly device-domain writes.
  const auto external_before = memory.coherency().current_epoch();
  const std::byte external_byte{0x7Fu};
  assert(memory.write_physical(ci_physical,
                               std::span<const std::byte>(&external_byte, 1u)));
  const auto external_after = memory.coherency().current_epoch();
  writes.clear();
  assert(memory.coherency().collect_exact_dirty_writes(
      external_before, external_after, writes));
  assert(writes.size() == 1u);
  assert(writes[0].ordering_domain == MemoryOrderingDomain::Device);

  std::cout << "xenon_memory_type_tests: ok\n";
  return 0;
}
