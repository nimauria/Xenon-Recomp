#include "memory/guest/address_space_internal.hpp"

namespace xenon::memory {

bool AddressSpace::page_has_mmio(std::uint32_t page_index) const {
  if (mmio_ranges_.empty()) return false;
  const auto page_base = page_index << kPageShift;
  const auto page_end = std::uint64_t{page_base} + kBasePageSize;

  // Ranges are sorted by base and non-overlapping, so only the last range
  // starting before this page ends can overlap the page. This keeps hot-page
  // publication O(log N) even with large device catalogues.
  const auto it = std::lower_bound(
      mmio_ranges_.begin(), mmio_ranges_.end(), page_end,
      [](const MmioRangeRef& range, std::uint64_t end) {
        return std::uint64_t{range->base} < end;
      });
  if (it == mmio_ranges_.begin()) return false;
  const auto& candidate = *std::prev(it);
  return std::uint64_t{candidate->base} + candidate->size > page_base;
}

std::uint64_t AddressSpace::make_hot_entry(std::uint32_t page_index) const {
  if (page_index >= kPageCount) return 0;
  const auto address = page_index << kPageShift;
  const bool slow = page_has_mmio(page_index);

  const auto direct = physical_alias_address(address);
  if (direct != 0xFFFFFFFFu && direct < kPhysicalMemorySize) {
    const auto protect = physical_protect(direct);
    return xenon::cpu::fast_memory::encode_page(
        direct >> kPageShift, has(protect, Protect::Read),
        has(protect, Protect::Write), has(protect, Protect::Execute), slow,
        has(protect, Protect::NoCache),
        has(protect, Protect::WriteCombine));
  }

  const auto* region = region_for(address);
  if (!region || region->kind == RegionKind::Mmio) {
    return slow || (region && region->kind == RegionKind::Mmio)
               ? xenon::cpu::fast_memory::kSlow
               : 0u;
  }
  if (region->kind != RegionKind::Virtual && region->kind != RegionKind::Xex) {
    return slow ? xenon::cpu::fast_memory::kSlow : 0u;
  }

  const auto& page = pages_[page_index];
  if (page.state != PageState::Committed ||
      page.physical_page == kInvalidPhysicalPage) {
    return slow ? xenon::cpu::fast_memory::kSlow : 0u;
  }

  return xenon::cpu::fast_memory::encode_page(
      page.physical_page, has(page.current_protect, Protect::Read),
      has(page.current_protect, Protect::Write),
      has(page.current_protect, Protect::Execute), slow,
      has(page.current_protect, Protect::NoCache),
      has(page.current_protect, Protect::WriteCombine));
}

void AddressSpace::publish_hot_entry(std::uint32_t page_index,
                                     std::uint64_t entry) {
  if (page_index >= kPageCount) return;
  if ((entry & (xenon::cpu::fast_memory::kMapped |
                xenon::cpu::fast_memory::kExecute)) ==
      (xenon::cpu::fast_memory::kMapped |
       xenon::cpu::fast_memory::kExecute)) {
    const auto physical_page = static_cast<std::uint32_t>(
        entry & xenon::cpu::fast_memory::kPhysicalPageMask);
    if (physical_page < executable_page_generations_.size()) {
      std::uint32_t expected = 0u;
      (void)executable_page_generations_[physical_page].compare_exchange_strong(
          expected, 1u, std::memory_order_release, std::memory_order_relaxed);
    }
  }
  hot_pages_[page_index].store(entry, std::memory_order_release);
}

void AddressSpace::publish_hot_page(std::uint32_t page_index) {
  if (page_index >= kPageCount) return;
  auto desired = make_hot_entry(page_index);
  if (!guest_aperture_ || !guest_aperture_->active()) {
    publish_hot_entry(page_index, desired);
    return;
  }

  const bool eligible =
      guest_aperture_->can_map_page(page_index) &&
      (desired & xenon::cpu::fast_memory::kMapped) != 0u &&
      (desired & (xenon::cpu::fast_memory::kSlow |
                  xenon::cpu::fast_memory::kNoCache |
                  xenon::cpu::fast_memory::kWriteCombine)) == 0u;
  const auto physical_page = static_cast<std::uint32_t>(
      desired & xenon::cpu::fast_memory::kPhysicalPageMask);
  if (eligible && guest_aperture_->mapping_matches(page_index, physical_page)) {
    publish_hot_entry(page_index,
                      desired | xenon::cpu::fast_memory::kDirectAperture);
    return;
  }
  if (!eligible && !guest_aperture_->page_mapped(page_index)) {
    publish_hot_entry(page_index, desired);
    return;
  }

  // First remove direct-aperture use from the published entry. Contexts created
  // after this point use compact physical translation. If an older context is
  // still alive it may have cached the former direct entry, so don't alter the
  // fixed host mapping until the old read-side population has drained.
  publish_hot_entry(page_index, desired);
  if (active_fast_readers_.load(std::memory_order_acquire) != 0u) return;

  if (eligible) {
    if (guest_aperture_->map_run(page_index, physical_page, 1u)) {
      publish_hot_entry(page_index,
                        desired | xenon::cpu::fast_memory::kDirectAperture);
    }
  } else {
    (void)guest_aperture_->clear_run(page_index, 1u);
  }
}

void AddressSpace::publish_hot_range(GuestAddress base, std::uint32_t size) {
  if (!size) return;
  const auto first = base >> kPageShift;
  const auto last64 = (std::uint64_t{base} + size - 1u) >> kPageShift;
  const auto last = static_cast<std::uint32_t>(
      std::min<std::uint64_t>(last64, kPageCount - 1u));
  const auto count = last - first + 1u;

  if (!guest_aperture_ || !guest_aperture_->active()) {
    for (std::uint32_t page = first; page <= last; ++page) {
      publish_hot_entry(page, make_hot_entry(page));
    }
    return;
  }

  // Small ranges: use stack buffers to avoid heap allocation for mobile efficiency
  constexpr std::uint32_t kMaxStackPages = 256u;
  std::uint64_t stack_desired[kMaxStackPages];
  std::uint8_t stack_needs_change[kMaxStackPages];
  
  std::uint64_t* desired = count <= kMaxStackPages ? stack_desired : new std::uint64_t[count];
  std::uint8_t* needs_change = count <= kMaxStackPages ? stack_needs_change : new std::uint8_t[count];
  std::memset(needs_change, 0, count);
  bool any_change = false;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto page = first + i;
    auto entry = make_hot_entry(page);
    desired[i] = entry;
    const bool eligible =
        guest_aperture_->can_map_page(page) &&
        (entry & xenon::cpu::fast_memory::kMapped) != 0u &&
        (entry & (xenon::cpu::fast_memory::kSlow |
                  xenon::cpu::fast_memory::kNoCache |
                  xenon::cpu::fast_memory::kWriteCombine)) == 0u;
    const auto physical_page = static_cast<std::uint32_t>(
        entry & xenon::cpu::fast_memory::kPhysicalPageMask);
    const bool matches =
        eligible && guest_aperture_->mapping_matches(page, physical_page);
    const bool change = matches ? false
                                : (eligible || guest_aperture_->page_mapped(page));
    needs_change[i] = change ? 1u : 0u;
    any_change |= change;

    // Unchanged direct aliases remain direct. Pages whose host mapping needs
    // alteration are first published without kDirectAperture, forming the
    // grace-period boundary for pre-existing contexts.
    publish_hot_entry(page, matches
                                ? entry | xenon::cpu::fast_memory::kDirectAperture
                                : entry);
  }

  if (!any_change ||
      active_fast_readers_.load(std::memory_order_acquire) != 0u) {
    if (count > kMaxStackPages) {
      delete[] desired;
      delete[] needs_change;
    }
    return;
  }

  std::uint32_t i = 0u;
  while (i < count) {
    if (!needs_change[i]) {
      ++i;
      continue;
    }
    const auto page = first + i;
    const auto entry = desired[i];
    const bool eligible =
        guest_aperture_->can_map_page(page) &&
        (entry & xenon::cpu::fast_memory::kMapped) != 0u &&
        (entry & (xenon::cpu::fast_memory::kSlow |
                  xenon::cpu::fast_memory::kNoCache |
                  xenon::cpu::fast_memory::kWriteCombine)) == 0u;

    if (!eligible) {
      std::uint32_t run = 1u;
      while (i + run < count && needs_change[i + run]) {
        const auto next = desired[i + run];
        if ((next & xenon::cpu::fast_memory::kMapped) != 0u &&
            (next & xenon::cpu::fast_memory::kSlow) == 0u) {
          break;
        }
        ++run;
      }
      (void)guest_aperture_->clear_run(page, run);
      i += run;
      continue;
    }

    const auto first_physical = static_cast<std::uint32_t>(
        entry & xenon::cpu::fast_memory::kPhysicalPageMask);
    std::uint32_t run = 1u;
    while (i + run < count && needs_change[i + run]) {
      const auto next = desired[i + run];
      const bool next_eligible =
          guest_aperture_->can_map_page(first + i + run) &&
          (next & xenon::cpu::fast_memory::kMapped) != 0u &&
          (next & (xenon::cpu::fast_memory::kSlow |
                   xenon::cpu::fast_memory::kNoCache |
                   xenon::cpu::fast_memory::kWriteCombine)) == 0u;
      const auto next_physical = static_cast<std::uint32_t>(
          next & xenon::cpu::fast_memory::kPhysicalPageMask);
      if (!next_eligible || next_physical != first_physical + run) break;
      ++run;
    }

    if (guest_aperture_->map_run(page, first_physical, run)) {
      for (std::uint32_t j = 0; j < run; ++j) {
        publish_hot_entry(first + i + j,
                          desired[i + j] |
                              xenon::cpu::fast_memory::kDirectAperture);
      }
    }
    i += run;
  }
  
  // Cleanup heap allocations for large ranges
  if (count > kMaxStackPages) {
    delete[] desired;
    delete[] needs_change;
  }
}

void AddressSpace::rebuild_hot_pages() {
  for (std::uint32_t page = 0; page < kPageCount; ++page) {
    publish_hot_page(page);
  }
}

}  // namespace xenon::memory
