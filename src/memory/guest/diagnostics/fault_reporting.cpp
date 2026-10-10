#include "memory/guest/address_space_internal.hpp"

namespace xenon::memory {

MemoryFaultInfo AddressSpace::make_fault_info(
    GuestAddress request_address, GuestAddress fault_address,
    std::size_t width, AccessKind access, FaultReason reason) const {
  std::lock_guard lock(mutex_);

  MemoryFaultInfo info{};
  info.request_address = request_address;
  info.fault_address = fault_address;
  info.width = width;
  info.access = access;
  info.reason = reason;

  const auto* region = region_for(fault_address);
  info.region_present = region != nullptr;
  if (region) {
    info.region_kind = region->kind;
    info.page_size = region->allocation_page_size;
  }

  const auto direct = physical_alias_address(fault_address);
  if (direct != 0xFFFFFFFFu) {
    info.region_present = true;
    info.mapped = direct < kPhysicalMemorySize;
    info.committed = info.mapped;
    info.page_state = info.mapped ? PageState::Committed : PageState::Free;
    if (info.mapped) {
      const auto& metadata =
          physical_page_metadata_[direct >> kPageShift];
      info.allocation_protect = metadata.allocation_protect;
      info.current_protect = metadata.current_protect;
    }
    info.physical_address = info.mapped ? direct : 0xFFFFFFFFu;
    if (has(info.current_protect, Protect::NoCache)) {
      info.memory_type = MemoryType::CacheInhibited;
      info.host_policy = HostMappingPolicy::TranslatedCacheInhibited;
    } else if (has(info.current_protect, Protect::WriteCombine)) {
      info.memory_type = MemoryType::WriteCombined;
      info.host_policy = HostMappingPolicy::TranslatedWriteCombined;
    } else {
      info.memory_type = MemoryType::NormalCached;
      info.host_policy = HostMappingPolicy::DefaultCachedShared;
    }
    return info;
  }

  const auto mmio = find_mmio_locked(fault_address, 1u);
  if ((region && region->kind == RegionKind::Mmio) || mmio) {
    info.mmio = true;
    info.memory_type = MemoryType::Device;
    info.host_policy = HostMappingPolicy::DeviceDispatcher;
    if (mmio) {
      info.mapped = true;
      info.committed = true;
      info.page_state = PageState::Committed;
      Protect device_protect = Protect::None;
      if (mmio->read) device_protect |= Protect::Read;
      if (mmio->write) device_protect |= Protect::Write;
      info.allocation_protect = device_protect;
      info.current_protect = device_protect;
    }
    // MMIO overlays can sit on an ordinary committed page. Keep the device
    // classification while still exposing the underlying page state below if
    // one exists.
    if (region && region->kind == RegionKind::Mmio) return info;
  }

  if (!region || (region->kind != RegionKind::Virtual &&
                  region->kind != RegionKind::Xex)) {
    return info;
  }

  const auto page_index = fault_address >> kPageShift;
  if (page_index >= pages_.size()) return info;
  const auto& page = pages_[page_index];
  info.page_state = page.state;
  info.allocation_protect = page.allocation_protect;
  info.current_protect = page.current_protect;
  info.committed = page.state == PageState::Committed;
  info.mapped = info.committed && page.physical_page != kInvalidPhysicalPage;
  if (info.mapped) {
    info.physical_address =
        page.physical_page * kBasePageSize +
        (fault_address & (kBasePageSize - 1u));
  }

  if (has(page.current_protect, Protect::NoCache)) {
    info.memory_type = MemoryType::CacheInhibited;
    info.host_policy = HostMappingPolicy::TranslatedCacheInhibited;
  } else if (has(page.current_protect, Protect::WriteCombine)) {
    info.memory_type = MemoryType::WriteCombined;
    info.host_policy = HostMappingPolicy::TranslatedWriteCombined;
  } else if (info.mmio) {
    info.memory_type = MemoryType::Device;
    info.host_policy = HostMappingPolicy::DeviceDispatcher;
  } else {
    info.memory_type = MemoryType::NormalCached;
    info.host_policy = HostMappingPolicy::DefaultCachedShared;
  }
  return info;
}

[[noreturn]] void AddressSpace::fault(GuestAddress address, std::size_t width,
                                      AccessKind access, FaultReason reason,
                                      const char* message) const {
  fault_at(address, address, width, access, reason, message);
}

[[noreturn]] void AddressSpace::fault_at(
    GuestAddress request_address, GuestAddress fault_address,
    std::size_t width, AccessKind access, FaultReason reason,
    const char* message) const {
  throw MemoryFault(make_fault_info(request_address, fault_address, width,
                                    access, reason),
                    message);
}

}  // namespace xenon::memory
