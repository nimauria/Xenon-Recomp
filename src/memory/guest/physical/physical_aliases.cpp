#include "memory/guest/address_space_internal.hpp"

namespace xenon::memory {

std::optional<GuestAddress> AddressSpace::physical_guest_alias(
    std::uint32_t physical_address,
    PhysicalPageClass page_class) noexcept {
  if (physical_address >= kPhysicalMemorySize) return std::nullopt;
  switch (page_class) {
    case PhysicalPageClass::Page64K:
      return kPhysical64KBase + physical_address;
    case PhysicalPageClass::Page16M:
      return kPhysical16MBase + physical_address;
    case PhysicalPageClass::Page4K: {
      if (physical_address < kPhysical4KViewOffset) return std::nullopt;
      const auto guest = std::uint64_t{kPhysical4KBase} + physical_address -
                         kPhysical4KViewOffset;
      if (guest > kPhysical4KHeapEnd) return std::nullopt;
      return static_cast<GuestAddress>(guest);
    }
  }
  return std::nullopt;
}

bool AddressSpace::map_virtual_to_physical(GuestAddress virtual_base,
                                           std::uint32_t physical_base,
                                           std::uint32_t size, Protect protect_value) {
  std::lock_guard lock(mutex_);
  if (!valid_memory_type_protection(protect_value) || !size || (virtual_base & (kBasePageSize - 1u)) ||
      (physical_base & (kBasePageSize - 1u)) || (size & (kBasePageSize - 1u))) return false;
  if (std::uint64_t(physical_base) + size > kPhysicalMemorySize) return false;
  if (physical_base < kPhysicalMemorySize &&
      std::uint64_t{physical_base} + size >
          kPhysicalAllocatableEndExclusive) {
    return false;
  }
  const auto* region = region_for(virtual_base);
  if (!region || region->kind != RegionKind::Virtual) return false;
  if (std::uint64_t(virtual_base) + size - 1u > region->end) return false;
  const auto first = virtual_base >> kPageShift;
  const auto count = size >> kPageShift;
  for (std::uint32_t i = 0; i < count; ++i) if (pages_[first + i].state != PageState::Free) return false;
  reclaim_retired_physical_pages();
  const auto physical_first = physical_base / kBasePageSize;
  for (std::uint32_t i = 0; i < count;) {
    if (physical_page_used_[physical_first + i] == kPhysicalRetired) {
      return false;
    }
    if (physical_page_used_[physical_first + i] != kPhysicalFree) {
      ++i;
      continue;
    }
    const auto run_first = i;
    while (i < count &&
           physical_page_used_[physical_first + i] == kPhysicalFree) {
      ++i;
    }
    if (!physical_allocator_->contains_free(physical_first + run_first,
                                            i - run_first)) {
      throw std::logic_error("physical allocator/free-state mismatch");
    }
  }

  // Mapping previously unowned physical RAM establishes explicit physical
  // ownership. Remove those pages from the free-range index before publishing
  // the guest mapping so the allocator can never hand the same frame out.
  for (std::uint32_t i = 0; i < count;) {
    if (physical_page_used_[physical_first + i] != kPhysicalFree) {
      ++i;
      continue;
    }
    const auto run_first = i;
    while (i < count &&
           physical_page_used_[physical_first + i] == kPhysicalFree) {
      ++i;
    }
    const auto run_count = i - run_first;
    if (!physical_allocator_->claim(physical_first + run_first, run_count)) {
      throw std::logic_error("failed to claim free physical range");
    }
    std::fill_n(physical_page_used_.begin() + physical_first + run_first,
                run_count, kPhysicalExplicit);
    for (std::uint32_t j = 0; j < run_count; ++j) {
      auto& metadata =
          physical_page_metadata_[physical_first + run_first + j];
      metadata.allocation_base_page = physical_first + run_first;
      metadata.allocation_page_count = run_count;
      metadata.allocation_page_size = kBasePageSize;
      metadata.allocation_protect = protect_value;
      metadata.current_protect = protect_value;
      metadata.explicit_allocation = true;
    }
  }

  for (std::uint32_t i = 0; i < count; ++i) {
    auto& p = pages_[first + i];
    p.physical_page = physical_first + i;
    p.allocation_base_page = first;
    p.allocation_page_count = count;
    p.allocation_protect = protect_value;
    p.current_protect = protect_value;
    p.state = PageState::Committed;
    p.kind = RegionKind::Virtual;
    p.explicit_physical_mapping = true;
    add_physical_mapping_ref(p.physical_page, first + i);
  }
  publish_hot_range(virtual_base, size);
  publish_physical_alias_range(physical_base, size);
  return true;
}

void AddressSpace::publish_physical_alias_range(
    std::uint32_t physical_address, std::uint32_t size) {
  if (!size || physical_address >= kPhysicalMemorySize) return;
  const auto end = std::min<std::uint64_t>(
      std::uint64_t{physical_address} + size, kPhysicalMemorySize);
  const auto first_page = physical_address >> kPageShift;
  const auto last_page = static_cast<std::uint32_t>((end - 1u) >> kPageShift);

  for (std::uint32_t page = first_page; page <= last_page; ++page) {
    const auto physical = page * kBasePageSize;
    if (physical < kGpuWritebackEnd - kGpuWritebackBase + 1u) {
      publish_hot_page((kGpuWritebackBase + physical) >> kPageShift);
    }
    if (physical <= kPhysical64KEnd - kPhysical64KBase) {
      publish_hot_page((kPhysical64KBase + physical) >> kPageShift);
    }
    if (physical <= kPhysical16MEnd - kPhysical16MBase) {
      publish_hot_page((kPhysical16MBase + physical) >> kPageShift);
    }
    if (physical >= kPhysical4KViewOffset) {
      const auto guest = std::uint64_t{kPhysical4KBase} +
                         physical - kPhysical4KViewOffset;
      if (guest <= kPhysical4KHeapEnd) {
        publish_hot_page(static_cast<std::uint32_t>(guest) >> kPageShift);
      }
    }
  }
}

std::uint32_t AddressSpace::get_physical_address(GuestAddress address) const {
  std::lock_guard lock(mutex_);
  const auto direct = physical_alias_address(address);
  if (direct != 0xFFFFFFFFu && direct < kPhysicalMemorySize) return direct;
  const auto* region = region_for(address);
  if (!region || (region->kind != RegionKind::Virtual && region->kind != RegionKind::Xex)) return 0xFFFFFFFFu;
  const auto& page = pages_[address >> kPageShift];
  if (page.state != PageState::Committed || page.physical_page == kInvalidPhysicalPage) return 0xFFFFFFFFu;
  return page.physical_page * kBasePageSize + (address & (kBasePageSize - 1u));
}

const std::byte* AddressSpace::physical_data(std::uint32_t physical_address) const {
  if (!initialized_ || physical_address >= kPhysicalMemorySize) return nullptr;
  return physical_->data() + physical_address;
}

bool AddressSpace::copy_physical_range(
    std::uint32_t physical_address, std::span<std::byte> destination) const {
  // Physical backing is lifetime-stable after initialize(). Use the same
  // atomic byte snapshot primitive as generated CPU block reads so GPU/APU
  // consumers may safely read concurrently with scalar CPU stores without
  // taking the global management lock. Mapping/allocation metadata is not
  // consulted by a raw physical snapshot.
  if (!initialized_ || std::uint64_t(physical_address) + destination.size() >
                           kPhysicalMemorySize) {
    return false;
  }
  xenon::cpu::detail::atomic_copy_from_guest(
      physical_->data() + physical_address, destination);
  return true;
}

bool AddressSpace::write_physical(std::uint32_t physical_address,
                                  std::span<const std::byte> source,
                                  std::uint64_t* published_epoch) {
  if (published_epoch) *published_epoch = 0u;
  if (source.empty()) return true;
  auto write = physical_write_span(
      physical_address, static_cast<std::uint32_t>(source.size()));
  if (!write || !write.write(0u, source)) return false;
  const auto epoch = write.commit();
  if (published_epoch) *published_epoch = epoch;
  return true;
}

bool AddressSpace::fill_physical(std::uint32_t physical_address,
                                 std::uint32_t size, std::byte value) {
  if (!size) return true;
  auto write = physical_write_span(physical_address, size);
  if (!write) return false;
  return write.fill(0u, size, value);
}

std::vector<GuestAddress> AddressSpace::dynamic_guest_aliases_for_physical(
    std::uint32_t physical_address) const {
  std::lock_guard lock(mutex_);
  std::vector<GuestAddress> aliases;
  if (!initialized_ || physical_address >= kPhysicalMemorySize) return aliases;
  const auto physical_page = physical_address >> kPageShift;
  const auto page_offset = physical_address & (kBasePageSize - 1u);
  const auto guest_pages = physical_reverse_mappings_->guests(physical_page);
  aliases.reserve(guest_pages.size());
  for (const auto guest_page : guest_pages) {
    aliases.push_back((guest_page << kPageShift) | page_offset);
  }
  std::sort(aliases.begin(), aliases.end());
  return aliases;
}

}  // namespace xenon::memory
