#include "memory/guest/address_space_internal.hpp"

namespace xenon::memory {

std::uint32_t AddressSpace::allocate_physical_page(bool top_down) {
  reclaim_retired_physical_pages();
  std::uint32_t page = kInvalidPhysicalPage;
  if (!physical_allocator_->allocate(1u, 1u, top_down, page,
                                     active_fast_readers_.load(std::memory_order_acquire))) {
    return kInvalidPhysicalPage;
  }
  physical_page_used_[page] = kPhysicalAnonymous;
  return page;
}

void AddressSpace::free_physical_page(std::uint32_t page) {
  if (page >= kPhysicalPageCount) return;
  // Dedicated GPU writeback pages stay reserved. Explicit physical allocations
  // are released only by free_physical().
  if (page < kHugePageSize / kBasePageSize) return;
  auto& ownership = physical_page_used_[page];
  if (ownership != kPhysicalAnonymous &&
      ownership != kPhysicalAnonymousPendingFree) {
    return;
  }
  if (physical_mapping_refs_[page] != 0u) {
    ownership = kPhysicalAnonymousPendingFree;
    return;
  }
  // Do not immediately recycle the backing page. A generated fast access may
  // have loaded the old hot translation immediately before it was unpublished.
  // Retired pages become allocator-visible only after all pre-existing
  // MemoryAccessContext read-side guards have drained.
  ownership = kPhysicalRetired;
  if (!physical_allocator_->retire(page, 1u)) {
    throw std::logic_error("physical retired-range overlap");
  }
}

void AddressSpace::reclaim_retired_physical_pages() {
  // Skip the grace-period latch entirely when there is nothing retired -
  // this function runs at the start of every physical page allocation, and
  // must not add a stall to the overwhelmingly common case where reclaim has
  // no work to do.
  if (!physical_allocator_->has_retired_ranges()) return;

  // Request a grace period before touching active_fast_readers_: once this
  // flag is visible, resolve_fast() declines the fast path for every NEW
  // access (falling back to the slow port), so active_fast_readers_ only has
  // to drain whatever accesses were already in flight, rather than depending
  // on the guest's own access pattern happening to produce a moment with
  // zero readers anywhere in the whole address space.
  //
  // This wait is deliberately BOUNDED, not indefinite: active_fast_readers_
  // can also be held by a genuinely long-lived standing reader by design
  // (see access_context()'s own doc comment and
  // memory_tests.cpp's read-side quiescence test, which keeps a
  // MemoryAccessContext alive across multiple calls on purpose, specifically
  // to prove retired pages are NOT recycled out from under it) - waiting
  // forever for such a reader to disappear would hang permanently instead of
  // correctly skipping reclaim this round, which is what the pre-existing
  // contract promises. A short bounded spin drains ordinary brief, transient
  // overlapping accesses (the actual problem under sustained multi-threaded
  // guest traffic) without breaking that contract: if the count is still
  // non-zero once the bound is reached, this falls back to the original
  // "skip reclaim, try again on the next allocation" behavior.
  reclaim_pending_.store(true, std::memory_order_release);
  constexpr int kMaxDrainSpins = 10000;
  bool drained = false;
  for (int spin = 0; spin < kMaxDrainSpins; ++spin) {
    if (active_fast_readers_.load(std::memory_order_acquire) == 0u) {
      drained = true;
      break;
    }
    std::this_thread::yield();
  }
  if (!drained) {
    reclaim_pending_.store(false, std::memory_order_release);
    return;
  }

  auto retired = physical_allocator_->take_retired_ranges();
  for (const auto& [first, count] : retired) {
    for (std::uint32_t i = 0; i < count; ++i) {
      if (physical_page_used_[first + i] != kPhysicalRetired ||
          physical_mapping_refs_[first + i] != 0u) {
        throw std::logic_error("retired physical range ownership mismatch");
      }
    }
    std::fill_n(physical_page_used_.begin() + first, count, kPhysicalFree);
    std::fill_n(physical_page_metadata_.begin() + first, count,
                PhysicalPageMetadata{});
    if (!physical_allocator_->release(first, count)) {
      throw std::logic_error("physical free-range allocator ownership mismatch");
    }
  }

  reclaim_pending_.store(false, std::memory_order_release);
}

void AddressSpace::add_physical_mapping_ref(std::uint32_t physical_page,
                                            std::uint32_t guest_page) {
  if (physical_page >= kPhysicalPageCount || guest_page >= kPageCount) {
    throw std::logic_error("physical mapping reference out of range");
  }
  if (!physical_reverse_mappings_->add(physical_page, guest_page)) {
    throw std::logic_error("duplicate physical reverse mapping");
  }
  ++physical_mapping_refs_[physical_page];
}

void AddressSpace::remove_physical_mapping_ref(std::uint32_t physical_page,
                                               std::uint32_t guest_page) {
  if (physical_page >= kPhysicalPageCount || guest_page >= kPageCount ||
      physical_mapping_refs_[physical_page] == 0u) {
    throw std::logic_error("invalid physical mapping reference removal");
  }
  if (!physical_reverse_mappings_->remove(physical_page, guest_page)) {
    throw std::logic_error("missing physical reverse mapping");
  }
  --physical_mapping_refs_[physical_page];
  if (physical_mapping_refs_[physical_page] == 0u &&
      physical_page_used_[physical_page] == kPhysicalAnonymousPendingFree) {
    free_physical_page(physical_page);
  }
}

bool AddressSpace::reserve_physical_run(std::uint32_t count,
                                        std::uint32_t alignment_pages,
                                        bool top_down,
                                        std::uint32_t minimum_page,
                                        std::uint32_t maximum_page_exclusive,
                                        std::uint32_t& out_first_page) {
  if (!count || count > kPhysicalPageCount || !alignment_pages ||
      !std::has_single_bit(alignment_pages) ||
      minimum_page >= maximum_page_exclusive) {
    return false;
  }
  reclaim_retired_physical_pages();
  if (!physical_allocator_->allocate(count, alignment_pages, top_down,
                                     minimum_page, maximum_page_exclusive,
                                     out_first_page,
                                     active_fast_readers_.load(std::memory_order_acquire))) {
    return false;
  }
  std::fill_n(physical_page_used_.begin() + out_first_page, count,
              kPhysicalExplicit);
  return true;
}

bool AddressSpace::allocate_physical(std::uint32_t size, std::uint32_t alignment,
                                     bool top_down, std::uint32_t& out_physical_address) {
  PhysicalAllocationOptions options{};
  options.alignment = alignment;
  options.top_down = top_down;
  return allocate_physical(size, options, out_physical_address);
}

bool AddressSpace::allocate_physical(
    std::uint32_t size, const PhysicalAllocationOptions& options,
    std::uint32_t& out_physical_address) {
  std::lock_guard lock(mutex_);
  if (!initialized_ || !size ||
      !valid_memory_type_protection(options.protect) ||
      !has(options.protect, Protect::Read)) {
    return false;
  }
  const auto page_size = physical_page_class_size(options.page_class);
  auto alignment = std::max(options.alignment ? options.alignment : page_size,
                            page_size);
  if (!std::has_single_bit(alignment)) return false;
  const auto rounded_size = align_up(size, page_size);
  const auto count = rounded_size / kBasePageSize;
  const auto alignment_pages = alignment / kBasePageSize;

  const auto minimum_address = std::max(options.minimum_address,
                                        kPhysicalSystemReserveSize);
  auto maximum_address = std::min(
      options.maximum_address, kPhysicalAllocatableEndExclusive - 1u);
  // MmAllocatePhysicalMemory-style allocations come from page-size-specific
  // Xbox physical heaps/views. The 4 KiB heap is the E-view and ends before
  // the 0xFFD00000 MMIO window, so a 4 KiB allocation may not be placed in
  // physical RAM that has no representable E-view guest address. The A/C
  // heaps cover the full physical backing (subject to the top 64 KiB guard).
  if (options.page_class == PhysicalPageClass::Page4K) {
    maximum_address =
        std::min(maximum_address, kPhysical4KAddressableEndInclusive);
  }
  if (minimum_address > maximum_address) return false;
  const auto minimum_page =
      align_up(minimum_address, kBasePageSize) / kBasePageSize;
  const auto maximum_page_exclusive = static_cast<std::uint32_t>(
      (std::uint64_t{maximum_address} + 1u) / kBasePageSize);
  if (minimum_page >= maximum_page_exclusive ||
      count > maximum_page_exclusive - minimum_page) {
    return false;
  }

  std::uint32_t first = 0;
  if (!reserve_physical_run(count, alignment_pages, options.top_down,
                            minimum_page, maximum_page_exclusive, first)) {
    return false;
  }
  out_physical_address = first * kBasePageSize;
  const auto physical_size = count * kBasePageSize;

  for (std::uint32_t i = 0; i < count; ++i) {
    auto& metadata = physical_page_metadata_[first + i];
    metadata.allocation_base_page = first;
    metadata.allocation_page_count = count;
    metadata.allocation_page_size = page_size;
    metadata.allocation_protect = options.protect;
    metadata.current_protect = options.protect;
    metadata.explicit_allocation = true;
  }

  if (options.zero_initialize) {
    const bool reservation_participant =
        begin_physical_write(out_physical_address, physical_size);
    std::memset(physical_->data() + out_physical_address, 0,
                std::size_t(count) * kBasePageSize);
    complete_physical_write(out_physical_address, physical_size,
                            reservation_participant,
                            xenon::cpu::MemoryOrderingDomain::Normal);
  }
  publish_physical_alias_range(out_physical_address, physical_size);
  return true;
}

bool AddressSpace::free_physical(std::uint32_t physical_base, std::uint32_t size) {
  std::lock_guard lock(mutex_);
  if (!size || (physical_base & (kBasePageSize - 1u)) ||
      std::uint64_t(physical_base) + size > kPhysicalMemorySize) return false;
  const auto first = physical_base / kBasePageSize;
  const auto& first_metadata = physical_page_metadata_[first];
  const auto requested_count = first_metadata.explicit_allocation
                                   ? align_up(size,
                                              first_metadata.allocation_page_size) /
                                         kBasePageSize
                                   : page_count_for(size);
  const auto count = first_metadata.explicit_allocation
                         ? first_metadata.allocation_page_count
                         : requested_count;
  if (first + count > kPhysicalPageCount || requested_count != count) {
    return false;
  }
  if (!first_metadata.explicit_allocation ||
      first_metadata.allocation_base_page != first ||
      first_metadata.allocation_page_count != count) {
    return false;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    if (physical_page_used_[first + i] != kPhysicalExplicit ||
        physical_mapping_refs_[first + i] != 0u ||
        !physical_page_metadata_[first + i].explicit_allocation ||
        physical_page_metadata_[first + i].allocation_base_page != first ||
        physical_page_metadata_[first + i].allocation_page_count != count) {
      return false;
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    if (has(physical_page_metadata_[first + i].current_protect,
            Protect::Execute)) {
      advance_executable_generation(first + i);
    }
    physical_page_used_[first + i] = kPhysicalRetired;
    physical_page_metadata_[first + i] = PhysicalPageMetadata{};
  }
  if (!physical_allocator_->retire(first, count)) {
    throw std::logic_error("physical retired-range overlap");
  }
  publish_physical_alias_range(physical_base, count * kBasePageSize);
  return true;
}

bool AddressSpace::protect_physical(std::uint32_t physical_base,
                                    std::uint32_t size,
                                    Protect protect_value,
                                    Protect* old_protect) {
  std::lock_guard lock(mutex_);
  if (!initialized_ || !size ||
      !valid_memory_type_protection(protect_value) ||
      std::uint64_t{physical_base} + size > kPhysicalMemorySize) {
    return false;
  }
  const auto requested_page = physical_base / kBasePageSize;
  const auto& first_requested = physical_page_metadata_[requested_page];
  if (!first_requested.explicit_allocation ||
      first_requested.allocation_base_page == kInvalidPhysicalPage ||
      !first_requested.allocation_page_count) {
    return false;
  }

  const auto allocation_base_page = first_requested.allocation_base_page;
  const auto allocation_page_count = first_requested.allocation_page_count;
  const auto allocation_page_size = first_requested.allocation_page_size;
  const auto allocation_base = allocation_base_page * kBasePageSize;
  const auto allocation_end =
      std::uint64_t{allocation_base} +
      std::uint64_t{allocation_page_count} * kBasePageSize;
  const auto requested_end_exclusive = std::uint64_t{physical_base} + size;
  if (physical_base < allocation_base ||
      requested_end_exclusive > allocation_end) {
    return false;
  }

  // MmSetAddressProtect-style changes obey the page class selected when the
  // physical allocation was created. A byte-sized request into a 64 KiB or
  // 16 MiB allocation changes that complete architectural page, while the
  // underlying metadata remains 4 KiB for the CPU fast path.
  const auto normalized_base = align_down(physical_base, allocation_page_size);
  const auto normalized_end_exclusive = std::min<std::uint64_t>(
      allocation_end,
      (requested_end_exclusive + allocation_page_size - 1u) &
          ~(std::uint64_t{allocation_page_size} - 1u));
  const auto first = normalized_base / kBasePageSize;
  const auto last = static_cast<std::uint32_t>(
      normalized_end_exclusive / kBasePageSize - 1u);
  if (old_protect) *old_protect =
      physical_page_metadata_[first].current_protect;
  for (std::uint32_t page = first; page <= last; ++page) {
    const auto& metadata = physical_page_metadata_[page];
    if (!metadata.explicit_allocation ||
        metadata.allocation_base_page != allocation_base_page ||
        metadata.allocation_page_count != allocation_page_count ||
        metadata.allocation_page_size != allocation_page_size) {
      return false;
    }
  }
  for (std::uint32_t page = first; page <= last; ++page) {
    const auto old_value = physical_page_metadata_[page].current_protect;
    if (has(old_value, Protect::Execute) !=
        has(protect_value, Protect::Execute)) {
      advance_executable_generation(page);
    }
    physical_page_metadata_[page].current_protect = protect_value;
  }
  publish_physical_alias_range(
      first * kBasePageSize, (last - first + 1u) * kBasePageSize);
  return true;
}

std::optional<PhysicalAllocationInfo> AddressSpace::query_physical_allocation(
    std::uint32_t physical_address) const {
  std::lock_guard lock(mutex_);
  if (!initialized_ || physical_address >= kPhysicalMemorySize) {
    return std::nullopt;
  }
  const auto page = physical_address / kBasePageSize;
  const auto& metadata = physical_page_metadata_[page];
  if (!metadata.explicit_allocation ||
      metadata.allocation_base_page == kInvalidPhysicalPage ||
      !metadata.allocation_page_count) {
    return std::nullopt;
  }
  return PhysicalAllocationInfo{
      physical_address,
      metadata.allocation_base_page * kBasePageSize,
      metadata.allocation_page_count * kBasePageSize,
      metadata.allocation_page_size,
      physical_page_class_from_size(metadata.allocation_page_size),
      metadata.allocation_protect,
      metadata.current_protect};
}

MemoryStatistics AddressSpace::memory_statistics() const {
  std::lock_guard lock(mutex_);
  MemoryStatistics result{};
  result.total_physical_pages = kPhysicalPageCount;
  for (const auto ownership : physical_page_used_) {
    switch (ownership) {
      case kPhysicalFree:
        ++result.available_physical_pages;
        break;
      case kPhysicalSystem:
        ++result.system_physical_pages;
        break;
      case kPhysicalAnonymous:
      case kPhysicalAnonymousPendingFree:
        ++result.anonymous_physical_pages;
        break;
      case kPhysicalExplicit:
        ++result.explicit_physical_pages;
        break;
      case kPhysicalRetired:
        // Retirement is a host-side quiescence delay, not guest-owned
        // memory. The allocator reclaims these before its next allocation,
        // so Xbox-visible available capacity includes them.
        ++result.available_physical_pages;
        ++result.retired_physical_pages;
        break;
      default:
        break;
    }
  }
  for (const auto& page : pages_) {
    if (page.state == PageState::Free) continue;
    if (page.kind == RegionKind::Xex) {
      if (page.state == PageState::Committed) ++result.image_pages;
      continue;
    }
    if (page.kind != RegionKind::Virtual &&
        page.kind != RegionKind::GpuWriteback) {
      continue;
    }
    ++result.reserved_virtual_pages;
    if (page.state == PageState::Committed) ++result.committed_virtual_pages;
  }
  return result;
}

}  // namespace xenon::memory
