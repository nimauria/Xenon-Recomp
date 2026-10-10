#include "memory/guest/address_space_internal.hpp"

namespace xenon::memory {

bool AddressSpace::range_is_allocatable(GuestAddress base, std::uint32_t size,
                                        std::uint32_t required_page_size) const {
  if (!size || !std::has_single_bit(required_page_size)) return false;
  const auto* region = region_for(base);
  if (!region || (region->kind != RegionKind::Virtual && region->kind != RegionKind::Xex)) return false;
  if (required_page_size != region->allocation_page_size) return false;
  if ((base & (required_page_size - 1u)) != 0 || (size & (required_page_size - 1u)) != 0) return false;
  const std::uint64_t end = std::uint64_t(base) + size - 1u;
  return end <= region->end;
}

bool AddressSpace::reserve_fixed(GuestAddress base, std::uint32_t size, Protect protect) {
  std::lock_guard lock(mutex_);
  if (!initialized_) return false;
  const auto* region = region_for(base);
  if (!region || !range_is_allocatable(base, size, region->allocation_page_size)) return false;
  return reserve_pages(base, size, protect, false);
}

bool AddressSpace::commit_fixed(GuestAddress base, std::uint32_t size,
                                Protect protect, bool zero_initialize) {
  std::lock_guard lock(mutex_);
  if (!initialized_) return false;
  const auto* region = region_for(base);
  if (!region || !range_is_allocatable(base, size, region->allocation_page_size)) return false;
  const std::uint32_t first = base >> kPageShift;
  const std::uint32_t count = size >> kPageShift;
  bool all_free = true;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (pages_[first + i].state != PageState::Free) {
      all_free = false;
      break;
    }
  }
  if (all_free && !reserve_pages(base, size, protect, false)) return false;
  return commit_pages(base, size, protect, zero_initialize);
}

bool AddressSpace::reserve_pages(GuestAddress base, std::uint32_t size, Protect protect,
                                 bool commit_now) {
  if (!valid_memory_type_protection(protect)) return false;
  const auto* region = region_for(base);
  if (!region) return false;
  const std::uint32_t first = base >> kPageShift;
  const std::uint32_t count = size >> kPageShift;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (pages_[first + i].state != PageState::Free) return false;
    if (region->kind == RegionKind::Xex) {
      const GuestAddress addr = base + i * kBasePageSize;
      const GuestAddress alias = addr < kXex4KBase ? addr + 0x10000000u : addr - 0x10000000u;
      if (pages_[alias >> kPageShift].state != PageState::Free) return false;
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    auto& page = pages_[first + i];
    page.state = PageState::Reserved;
    page.allocation_base_page = first;
    page.allocation_page_count = count;
    page.allocation_protect = protect;
    page.current_protect = protect;
    page.kind = region->kind;
    if (region->kind == RegionKind::Xex) {
      const GuestAddress addr = base + i * kBasePageSize;
      const GuestAddress alias = addr < kXex4KBase ? addr + 0x10000000u : addr - 0x10000000u;
      auto& alias_page = pages_[alias >> kPageShift];
      alias_page = page;
      alias_page.allocation_base_page =
          (base < kXex4KBase ? base + 0x10000000u : base - 0x10000000u) >> kPageShift;
    }
  }
  publish_hot_range(base, size);
  if (region->kind == RegionKind::Xex) {
    const auto alias_base = base < kXex4KBase ? base + 0x10000000u
                                               : base - 0x10000000u;
    publish_hot_range(alias_base, size);
  }
  return !commit_now || commit_pages(base, size, protect, true);
}

bool AddressSpace::commit_pages(GuestAddress base, std::uint32_t size,
                                Protect protect, bool zero_initialize) {
  if (!valid_memory_type_protection(protect)) return false;
  const auto* region = region_for(base);
  if (!region) return false;
  const std::uint32_t first = base >> kPageShift;
  const std::uint32_t count = size >> kPageShift;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto& page = pages_[first + i];
    if (page.state == PageState::Free || page.permanent_guard) return false;
  }

  std::vector<std::uint32_t> newly_allocated;
  newly_allocated.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    auto& page = pages_[first + i];
    if (page.state == PageState::Committed) {
      page.current_protect = protect;
      if (region->kind == RegionKind::Xex) {
        const GuestAddress addr = base + i * kBasePageSize;
        const GuestAddress alias = addr < kXex4KBase
                                       ? addr + 0x10000000u
                                       : addr - 0x10000000u;
        pages_[alias >> kPageShift].current_protect = protect;
      }
      continue;
    }
    const auto physical_page = allocate_physical_page(false);
    if (physical_page == kInvalidPhysicalPage) {
      for (std::uint32_t j = 0; j < i; ++j) {
        auto& rollback = pages_[first + j];
        if (rollback.physical_page == kInvalidPhysicalPage ||
            std::find(newly_allocated.begin(), newly_allocated.end(),
                      rollback.physical_page) == newly_allocated.end()) {
          continue;
        }
        const auto rollback_physical = rollback.physical_page;
        remove_physical_mapping_ref(rollback_physical, first + j);
        if (region->kind == RegionKind::Xex) {
          const GuestAddress addr = base + j * kBasePageSize;
          const GuestAddress alias = addr < kXex4KBase
                                         ? addr + 0x10000000u
                                         : addr - 0x10000000u;
          const auto alias_page_index = alias >> kPageShift;
          auto& alias_page = pages_[alias_page_index];
          remove_physical_mapping_ref(rollback_physical, alias_page_index);
          alias_page.physical_page = kInvalidPhysicalPage;
          alias_page.state = PageState::Reserved;
        }
        rollback.physical_page = kInvalidPhysicalPage;
        rollback.state = PageState::Reserved;
        free_physical_page(rollback_physical);
      }
      return false;
    }
    newly_allocated.push_back(physical_page);
    page.physical_page = physical_page;
    page.state = PageState::Committed;
    page.current_protect = protect;
    add_physical_mapping_ref(physical_page, first + i);
    if (zero_initialize) {
      const auto physical_address = physical_page * kBasePageSize;
      const bool reservation_participant =
          begin_physical_write(physical_address, kBasePageSize);
      std::memset(physical_->data() +
                      std::size_t(physical_page) * kBasePageSize,
                  0, kBasePageSize);
      complete_physical_write(physical_address, kBasePageSize,
                              reservation_participant,
                              xenon::cpu::MemoryOrderingDomain::Normal);
    }

    if (region->kind == RegionKind::Xex) {
      const GuestAddress addr = base + i * kBasePageSize;
      const GuestAddress alias = addr < kXex4KBase ? addr + 0x10000000u : addr - 0x10000000u;
      const auto alias_page_index = alias >> kPageShift;
      auto& alias_page = pages_[alias_page_index];
      alias_page.physical_page = physical_page;
      alias_page.state = PageState::Committed;
      alias_page.current_protect = protect;
      add_physical_mapping_ref(physical_page, alias_page_index);
    }
  }
  publish_hot_range(base, size);
  if (region->kind == RegionKind::Xex) {
    const auto alias_base = base < kXex4KBase ? base + 0x10000000u
                                               : base - 0x10000000u;
    publish_hot_range(alias_base, size);
  }
  return true;
}

bool AddressSpace::allocate(std::uint32_t size, std::uint32_t alignment, Protect protect,
                            bool top_down, GuestAddress& out_address,
                            std::optional<std::uint32_t> requested_page_size,
                            VirtualAllocationOptions options) {
  std::lock_guard lock(mutex_);
  const std::uint32_t _orig_size = size, _orig_alignment = alignment;
  if (!initialized_ || !size) {
    xenon::logging::append_probe_log("allocate_early_fail_diag.log",
                                     "EARLY FAIL: initialized_=%d size=%u\n", initialized_ ? 1 : 0,
                                     size);
    return false;
  }
  const std::uint32_t page_size = requested_page_size.value_or(kBasePageSize);
  const RegionDescriptor* region = nullptr;
  for (const auto& candidate : kRegions) {
    if (candidate.kind == RegionKind::Virtual && candidate.allocation_page_size == page_size) {
      region = &candidate;
      break;
    }
  }
  if (!region) {
    xenon::logging::append_probe_log("allocate_early_fail_diag.log",
                                     "NO REGION FAIL: page_size=%u\n", page_size);
    return false;
  }
  alignment = std::max(alignment ? alignment : page_size, page_size);
  if (!std::has_single_bit(alignment)) {
    xenon::logging::append_probe_log("allocate_early_fail_diag.log",
                                     "BAD ALIGNMENT FAIL: alignment=%u (orig=%u)\n", alignment,
                                     _orig_alignment);
    return false;
  }
  size = align_up(size, page_size);
  {
    static std::atomic<int> _alloc_entry_count{0};
    if (_alloc_entry_count.fetch_add(1) < 5 ||
        (_alloc_entry_count.load() % 1000) == 0) {
      xenon::logging::append_probe_log(
          "allocate_early_fail_diag.log",
          "ALLOCATE ENTRY #%d: region=[0x%08X-0x%08X] page_size=%u "
          "orig_size=%u rounded_size=%u alignment=%u top_down=%d\n",
          _alloc_entry_count.load(), region->base, region->end, page_size, _orig_size, size,
          alignment, top_down ? 1 : 0);
    }
  }

  // Defensive bound: first+count must never exceed pages_.size() before any
  // pages_[...] index below runs. This is not a "this can't happen" belt -
  // real corruption reaching this vector (from anywhere in the process, not
  // necessarily this function's own arithmetic) has been observed in
  // practice as an unhandled MSVC debug-CRT "vector subscript out of range"
  // crash deep inside this exact loop; failing the allocation cleanly here
  // (with a diagnostic identifying the actual bad index) is strictly better
  // than propagating an out-of-bounds access into undefined behavior.
  const auto page_index_in_bounds = [this](std::uint32_t first, std::uint32_t count) noexcept {
    if (count == 0u) return true;
    const auto last = static_cast<std::uint64_t>(first) + (count - 1u);
    if (last >= pages_.size()) {
      std::fprintf(stderr,
                   "[AddressSpace::allocate] out-of-bounds page index: first=0x%08X count=0x%08X "
                   "last=0x%016llX pages_.size()=0x%016zX\n",
                   first, count, static_cast<unsigned long long>(last), pages_.size());
      // This is never a legitimate "allocation full"/OOM condition - an
      // out-of-bounds page index reaching here means corruption occurred
      // somewhere. In debug/test builds, fail loudly (this file's
      // SuppressBlockingCrtDialogs above already redirects any debug-CRT
      // assert to stderr instead of a blocking dialog, so this aborts
      // promptly with the reason in the log rather than hanging). Release
      // builds still degrade to the quiet `return false` below, which the
      // existing allocation-failure paths already handle safely.
      assert(false && "AddressSpace::allocate: out-of-bounds page index (see stderr diagnostic)");
      return false;
    }
    return true;
  };

  if (!top_down) {
    for (std::uint64_t candidate = align_up(region->base, alignment);
         candidate + size - 1u <= region->end; candidate += alignment) {
      const auto first = static_cast<std::uint32_t>(candidate) >> kPageShift;
      const auto count = size >> kPageShift;
      if (!page_index_in_bounds(first, count)) return false;
      bool free = true;
      for (std::uint32_t i = 0; i < count; ++i) {
        if (pages_[first + i].state != PageState::Free) { free = false; break; }
      }
      if (free) {
        out_address = static_cast<GuestAddress>(candidate);
        if (!reserve_pages(out_address, size, protect, false)) {
          xenon::logging::append_probe_log("allocate_early_fail_diag.log",
                                           "RESERVE_PAGES FAIL: addr=0x%08X size=%u\n",
                                           (unsigned)out_address, size);
          return false;
        }
        if (!options.commit) return true;
        const bool committed = commit_pages(out_address, size, protect,
                                            options.zero_initialize);
        if (!committed) {
          xenon::logging::append_probe_log("allocate_early_fail_diag.log",
                                           "COMMIT_PAGES FAIL: addr=0x%08X size=%u\n",
                                           (unsigned)out_address, size);
        }
        return committed;
      }
    }
  } else {
    const std::uint64_t high_start = std::uint64_t(region->end) + 1u - size;
    for (std::uint64_t candidate = align_down(static_cast<std::uint32_t>(high_start), alignment);;
         candidate -= alignment) {
      if (candidate < region->base) break;
      const auto first = static_cast<std::uint32_t>(candidate) >> kPageShift;
      const auto count = size >> kPageShift;
      if (!page_index_in_bounds(first, count)) return false;
      bool free = true;
      for (std::uint32_t i = 0; i < count; ++i) {
        if (pages_[first + i].state != PageState::Free) { free = false; break; }
      }
      if (free) {
        out_address = static_cast<GuestAddress>(candidate);
        if (!reserve_pages(out_address, size, protect, false)) return false;
        return !options.commit ||
               commit_pages(out_address, size, protect,
                            options.zero_initialize);
      }
      if (candidate < region->base + alignment) break;
    }
  }
  xenon::logging::append_probe_log("virt_alloc_fail_diag.log",
                                   "VIRT_ALLOC_FAIL size=%u alignment=%u top_down=%d\n", size,
                                   alignment, top_down ? 1 : 0);
  return false;
}

}  // namespace xenon::memory#include "xenon/logging/probe_log.hpp"

