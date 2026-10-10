#include "memory/guest/address_space_internal.hpp"

namespace xenon::memory {

void AddressSpace::advance_executable_generation(
    std::uint32_t physical_page) noexcept {
  if (physical_page >= executable_page_generations_.size()) return;
  auto generation = executable_page_generations_[physical_page].load(
      std::memory_order_relaxed);
  for (;;) {
    const auto next = generation == 0u
                          ? 1u
                          : generation ==
                                    (std::numeric_limits<std::uint32_t>::max)()
                                ? 1u
                                : generation + 1u;
    if (executable_page_generations_[physical_page].compare_exchange_weak(
            generation, next, std::memory_order_release,
            std::memory_order_relaxed)) {
      return;
    }
  }
}

xenon::cpu::ExecutablePageStamp AddressSpace::executable_page_stamp(
    GuestAddress address) const noexcept {
  const auto page_index = address >> kPageShift;
  if (page_index >= hot_pages_.size()) return {};
  const auto entry = hot_pages_[page_index].load(std::memory_order_acquire);
  if ((entry & (xenon::cpu::fast_memory::kMapped |
                xenon::cpu::fast_memory::kExecute)) !=
      (xenon::cpu::fast_memory::kMapped |
       xenon::cpu::fast_memory::kExecute)) {
    return {};
  }
  const auto physical_page = static_cast<std::uint32_t>(
      entry & xenon::cpu::fast_memory::kPhysicalPageMask);
  if (physical_page >= executable_page_generations_.size()) return {};
  const auto generation = executable_page_generations_[physical_page].load(
      std::memory_order_acquire);
  if (!generation) return {};
  return {physical_page, generation};
}

std::uint32_t AddressSpace::executable_generation(
    GuestAddress address) const noexcept {
  return executable_page_stamp(address).generation;
}

MemoryTypeInfo AddressSpace::memory_type_info(
    GuestAddress address) const noexcept {
  MemoryTypeInfo info{};
  const auto page_index = address >> kPageShift;
  if (page_index >= hot_pages_.size()) return info;

  const auto entry = hot_pages_[page_index].load(std::memory_order_acquire);
  info.mapped = (entry & xenon::cpu::fast_memory::kMapped) != 0u;
  info.executable =
      (entry & xenon::cpu::fast_memory::kExecute) != 0u;

  if (entry & xenon::cpu::fast_memory::kSlow) {
    if (const auto* region = region_for(address);
        region && region->kind == RegionKind::Mmio) {
      info.type = MemoryType::Device;
      info.host_policy = HostMappingPolicy::DeviceDispatcher;
      info.mapped = true;
      info.executable = false;
      info.direct_aperture_eligible = false;
      info.explicit_device_visibility = true;
      return info;
    }
    if (snapshot_mmio(address, 1u)) {
      info.type = MemoryType::Device;
      info.host_policy = HostMappingPolicy::DeviceDispatcher;
      info.mapped = true;
      info.executable = false;
      info.direct_aperture_eligible = false;
      info.explicit_device_visibility = true;
      return info;
    }
  }

  if (!info.mapped) return info;
  if (entry & xenon::cpu::fast_memory::kNoCache) {
    info.type = MemoryType::CacheInhibited;
    info.host_policy = HostMappingPolicy::TranslatedCacheInhibited;
    info.direct_aperture_eligible = false;
    info.explicit_device_visibility = true;
    return info;
  }
  if (entry & xenon::cpu::fast_memory::kWriteCombine) {
    info.type = MemoryType::WriteCombined;
    info.host_policy = HostMappingPolicy::TranslatedWriteCombined;
    info.direct_aperture_eligible = false;
    info.explicit_device_visibility = true;
    return info;
  }

  info.type = MemoryType::NormalCached;
  info.host_policy = HostMappingPolicy::DefaultCachedShared;
  info.direct_aperture_eligible = true;
  info.explicit_device_visibility = false;
  return info;
}

void AddressSpace::barrier(xenon::cpu::BarrierKind kind) {
  xenon::cpu::host_memory_ordering::apply(kind);
}

void AddressSpace::zero_cache_block(GuestAddress address, std::uint32_t bytes) {
  auto access = access_context();
  access.zero_cache_block(address, bytes);
}

void AddressSpace::instruction_cache_invalidate(GuestAddress address) {
  const auto page_index = address >> kPageShift;
  if (page_index < hot_pages_.size()) {
    const auto entry = hot_pages_[page_index].load(std::memory_order_acquire);
    if ((entry & xenon::cpu::fast_memory::kMapped) != 0u) {
      const auto physical_page = static_cast<std::uint32_t>(
          entry & xenon::cpu::fast_memory::kPhysicalPageMask);
      if (physical_page < executable_page_generations_.size() &&
          executable_page_generations_[physical_page].load(
              std::memory_order_acquire) != 0u) {
        advance_executable_generation(physical_page);
      }
    }
  }
  std::vector<InvalidationCallback> callbacks;
  {
    std::lock_guard lock(mutex_);
    callbacks.reserve(invalidation_callbacks_.size());
    for (const auto& [id, callback] : invalidation_callbacks_) {
      (void)id;
      callbacks.push_back(callback);
    }
  }
  for (auto& callback : callbacks) if (callback) callback(address);
}

}  // namespace xenon::memory
