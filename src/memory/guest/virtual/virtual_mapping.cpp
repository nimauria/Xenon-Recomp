#include "memory/guest/address_space_internal.hpp"

namespace xenon::memory {

bool AddressSpace::decommit(GuestAddress base, std::uint32_t size) {
  std::lock_guard lock(mutex_);
  if (!size) return false;
  const auto* region = region_for(base);
  if (!region || (region->kind != RegionKind::Virtual &&
                  region->kind != RegionKind::Xex)) {
    return false;
  }
  const auto requested_end = std::uint64_t{base} + size - 1u;
  if (requested_end > region->end) return false;

  // Management operations use the architectural page size of the selected
  // Xbox heap, even though the hot translation table remains 4 KiB. This
  // prevents a 64 KiB virtual/XEX page from being externally split into
  // sixteen independently committed 4 KiB states.
  const auto native_page_size = region->allocation_page_size;
  const auto normalized_base = align_down(base, native_page_size);
  const auto normalized_end =
      std::min<std::uint64_t>(
          region->end,
          (requested_end | (std::uint64_t{native_page_size} - 1u)));
  const std::uint32_t first = normalized_base >> kPageShift;
  const auto last = static_cast<std::uint32_t>(normalized_end >> kPageShift);
  const std::uint32_t count = last - first + 1u;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (pages_[first + i].state == PageState::Free ||
        pages_[first + i].permanent_guard) {
      return false;
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    auto& page = pages_[first + i];
    if (page.state == PageState::Committed &&
        page.physical_page != kInvalidPhysicalPage) {
      const auto physical_page = page.physical_page;
      if (has(page.current_protect, Protect::Execute)) {
        advance_executable_generation(physical_page);
      }
      remove_physical_mapping_ref(physical_page, first + i);
      if (region->kind == RegionKind::Xex) {
        const GuestAddress addr = (first + i) << kPageShift;
        const GuestAddress alias = addr < kXex4KBase
                                       ? addr + 0x10000000u
                                       : addr - 0x10000000u;
        const auto alias_page_index = alias >> kPageShift;
        auto& alias_page = pages_[alias_page_index];
        remove_physical_mapping_ref(physical_page, alias_page_index);
        alias_page.physical_page = kInvalidPhysicalPage;
        alias_page.state = PageState::Reserved;
      }
      if (!page.explicit_physical_mapping) {
        free_physical_page(physical_page);
      }
      page.physical_page = kInvalidPhysicalPage;
      page.explicit_physical_mapping = false;
    }
    page.state = PageState::Reserved;
  }
  const auto normalized_size = count * kBasePageSize;
  publish_hot_range(normalized_base, normalized_size);
  if (region->kind == RegionKind::Xex) {
    const auto alias_base = normalized_base < kXex4KBase
                                ? normalized_base + 0x10000000u
                                : normalized_base - 0x10000000u;
    publish_hot_range(alias_base, normalized_size);
  }
  return true;
}

bool AddressSpace::release(GuestAddress allocation_base) {
  std::lock_guard lock(mutex_);
  const auto page_index = allocation_base >> kPageShift;
  auto page = pages_[page_index];
  if (page.state == PageState::Free || page.permanent_guard ||
      page.allocation_base_page != page_index || !page.allocation_page_count) return false;
  const auto* region = region_for(allocation_base);
  if (!region || (region->kind != RegionKind::Virtual && region->kind != RegionKind::Xex)) return false;
  const auto allocation_page_count = page.allocation_page_count;
  for (std::uint32_t i = 0; i < allocation_page_count; ++i) {
    auto& p = pages_[page_index + i];
    if (p.state == PageState::Committed &&
        p.physical_page != kInvalidPhysicalPage) {
      const auto physical_page = p.physical_page;
      if (has(p.current_protect, Protect::Execute)) {
        advance_executable_generation(physical_page);
      }
      remove_physical_mapping_ref(physical_page, page_index + i);
      if (region->kind == RegionKind::Xex) {
        const GuestAddress addr = (page_index + i) << kPageShift;
        const GuestAddress alias = addr < kXex4KBase
                                       ? addr + 0x10000000u
                                       : addr - 0x10000000u;
        const auto alias_page_index = alias >> kPageShift;
        remove_physical_mapping_ref(physical_page, alias_page_index);
        pages_[alias_page_index] = Page{};
      }
      if (!p.explicit_physical_mapping) {
        free_physical_page(physical_page);
      }
    } else if (region->kind == RegionKind::Xex) {
      const GuestAddress addr = (page_index + i) << kPageShift;
      const GuestAddress alias = addr < kXex4KBase
                                     ? addr + 0x10000000u
                                     : addr - 0x10000000u;
      pages_[alias >> kPageShift] = Page{};
    }
    p = Page{};
  }
  const auto allocation_size = allocation_page_count * kBasePageSize;
  publish_hot_range(allocation_base, allocation_size);
  if (region->kind == RegionKind::Xex) {
    const auto alias_base = allocation_base < kXex4KBase
                                ? allocation_base + 0x10000000u
                                : allocation_base - 0x10000000u;
    publish_hot_range(alias_base, allocation_size);
  }
  return true;
}

bool AddressSpace::protect(GuestAddress base, std::uint32_t size,
                           Protect protect_value, Protect* old_protect) {
  std::lock_guard lock(mutex_);
  if (!size || !valid_memory_type_protection(protect_value)) return false;
  const auto* region = region_for(base);
  if (!region || (region->kind != RegionKind::Virtual &&
                  region->kind != RegionKind::Xex)) {
    return false;
  }
  const auto requested_end = std::uint64_t{base} + size - 1u;
  if (requested_end > region->end) return false;

  const auto native_page_size = region->allocation_page_size;
  const auto normalized_base = align_down(base, native_page_size);
  const auto normalized_end =
      std::min<std::uint64_t>(
          region->end,
          requested_end | (std::uint64_t{native_page_size} - 1u));
  const std::uint32_t first = normalized_base >> kPageShift;
  const auto last = static_cast<std::uint32_t>(normalized_end >> kPageShift);
  if (last >= pages_.size()) return false;

  const auto allocation_base_page = pages_[first].allocation_base_page;
  for (std::uint32_t i = first; i <= last; ++i) {
    if (pages_[i].state != PageState::Committed || pages_[i].permanent_guard ||
        pages_[i].allocation_base_page != allocation_base_page) {
      return false;
    }
  }
  if (old_protect) *old_protect = pages_[first].current_protect;
  for (std::uint32_t i = first; i <= last; ++i) {
    const auto old_value = pages_[i].current_protect;
    const bool execute_changed =
        has(old_value, Protect::Execute) != has(protect_value, Protect::Execute);
    if (execute_changed && pages_[i].physical_page != kInvalidPhysicalPage) {
      // An Execute -> NX -> Execute cycle must never resurrect a translation
      // that was compiled under the old protection epoch. The generation is
      // physical so every guest alias observes the same invalidation.
      advance_executable_generation(pages_[i].physical_page);
    }
    pages_[i].current_protect = protect_value;
    if (pages_[i].kind == RegionKind::Xex) {
      GuestAddress addr = i << kPageShift;
      GuestAddress alias = addr < kXex4KBase ? addr + 0x10000000u : addr - 0x10000000u;
      pages_[alias >> kPageShift].current_protect = protect_value;
    }
    publish_hot_page(i);
    if (pages_[i].kind == RegionKind::Xex) {
      const GuestAddress addr = i << kPageShift;
      const GuestAddress alias = addr < kXex4KBase ? addr + 0x10000000u : addr - 0x10000000u;
      publish_hot_page(alias >> kPageShift);
    }
  }
  return true;
}

std::optional<MappingInfo> AddressSpace::query(GuestAddress address) const {
  std::lock_guard lock(mutex_);
  const auto* region = region_for(address);
  if (!region) return std::nullopt;
  if (region->kind == RegionKind::PhysicalAlias || region->kind == RegionKind::GpuWriteback) {
    const auto physical = physical_alias_address(address);
    if (physical == 0xFFFFFFFFu || physical >= kPhysicalMemorySize) {
      return std::nullopt;
    }
    const auto physical_page = physical >> kPageShift;
    const auto& metadata = physical_page_metadata_[physical_page];
    const auto allocation_protect = metadata.allocation_protect;
    const auto current_protect = metadata.current_protect;
    GuestAddress allocation_base = region->base;
    std::uint32_t allocation_size = region->end - region->base + 1u;
    std::uint32_t region_size = region->end - address + 1u;
    std::uint32_t page_size = region->allocation_page_size;
    if (metadata.explicit_allocation) {
      const auto physical_base = metadata.allocation_base_page * kBasePageSize;
      const auto physical_end =
          physical_base + metadata.allocation_page_count * kBasePageSize;
      page_size = metadata.allocation_page_size;
      allocation_size = metadata.allocation_page_count * kBasePageSize;
      if (region->kind == RegionKind::GpuWriteback) {
        allocation_base = kGpuWritebackBase + physical_base;
      } else if (address >= kPhysical64KBase && address <= kPhysical64KEnd) {
        allocation_base = kPhysical64KBase + physical_base;
      } else if (address >= kPhysical16MBase && address <= kPhysical16MEnd) {
        allocation_base = kPhysical16MBase + physical_base;
      } else if (physical_base >= kPhysical4KViewOffset) {
        allocation_base = kPhysical4KBase +
                          (physical_base - kPhysical4KViewOffset);
      }
      const auto allocation_last_page =
          metadata.allocation_base_page + metadata.allocation_page_count;
      auto run_last_page = physical_page;
      while (run_last_page + 1u < allocation_last_page &&
             physical_page_metadata_[run_last_page + 1u].current_protect ==
                 current_protect) {
        ++run_last_page;
      }
      const auto protection_run_end =
          std::uint64_t{run_last_page + 1u} * kBasePageSize;
      region_size = static_cast<std::uint32_t>(std::min<std::uint64_t>(
          protection_run_end - physical,
          std::min<std::uint64_t>(std::uint64_t{physical_end} - physical,
                                  std::uint64_t{region->end} - address + 1u)));
    }
    return MappingInfo{address, physical, allocation_base, allocation_size,
                       region_size, page_size, PageState::Committed, allocation_protect,
                       current_protect, region->kind};
  }
  if (region->kind == RegionKind::Mmio) {
    return MappingInfo{address, 0xFFFFFFFFu, region->base, region->end - region->base + 1u,
                       region->end - address + 1u, region->allocation_page_size,
                       PageState::Committed, kReadWrite, kReadWrite, region->kind};
  }
  const auto& page = pages_[address >> kPageShift];
  MappingInfo info{};
  info.guest_address = address;
  info.physical_address =
      page.state == PageState::Committed &&
              page.physical_page != kInvalidPhysicalPage
          ? page.physical_page * kBasePageSize +
                (address & (kBasePageSize - 1u))
          : 0xFFFFFFFFu;
  info.allocation_base = page.allocation_base_page << kPageShift;
  info.allocation_size = page.allocation_page_count * kBasePageSize;
  info.page_size = region->allocation_page_size;
  info.state = page.state;
  info.allocation_protect = page.allocation_protect;
  info.current_protect = page.current_protect;
  info.kind = region->kind;
  const auto start = address >> kPageShift;
  const auto region_last = region->end >> kPageShift;
  if (page.state != PageState::Free) {
    std::uint32_t run = 0;
    for (std::uint32_t i = start; i <= region_last; ++i) {
      const auto& p = pages_[i];
      if (p.state != page.state ||
          p.current_protect != page.current_protect ||
          p.kind != page.kind ||
          p.allocation_base_page != page.allocation_base_page) {
        break;
      }
      ++run;
    }
    info.region_size = run * kBasePageSize;
  } else {
    // MEMORY_BASIC_INFORMATION-style queries must describe the free run too.
    // Keep the byte-accurate queried start while stopping at the next occupied
    // page or the architectural heap boundary.
    std::uint32_t run_pages = 0;
    for (std::uint32_t i = start; i <= region_last; ++i) {
      if (pages_[i].state != PageState::Free) break;
      ++run_pages;
    }
    if (run_pages) {
      const auto run_end =
          std::min<std::uint64_t>(
              std::uint64_t{region->end} + 1u,
              std::uint64_t{start + run_pages} * kBasePageSize);
      info.region_size = static_cast<std::uint32_t>(run_end - address);
    }
  }
  return info;
}

}  // namespace xenon::memory
