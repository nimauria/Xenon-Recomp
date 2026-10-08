#include "memory/guest/address_space_internal.hpp"

namespace xenon::memory {

bool AddressSpace::validate_invariants(std::string* error) const {
  std::lock_guard lock(mutex_);
  return validate_invariants_locked(error);
}

bool AddressSpace::validate_invariants_locked(std::string* error) const {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  if (!initialized_) return fail("address space is not initialized");

  for (std::uint32_t physical_page = 0; physical_page < kPhysicalPageCount;
       ++physical_page) {
    const auto reverse_count =
        physical_reverse_mappings_->count(physical_page);
    if (physical_mapping_refs_[physical_page] != reverse_count) {
      return fail("physical mapping refcount/reverse-map mismatch at page " +
                  std::to_string(physical_page));
    }

    const auto ownership = physical_page_used_[physical_page];
    const bool allocator_free =
        physical_allocator_->contains_free(physical_page, 1u);
    const bool allocator_retired =
        physical_allocator_->contains_retired(physical_page, 1u);
    if (ownership == kPhysicalFree) {
      if (physical_mapping_refs_[physical_page] != 0u || !allocator_free ||
          allocator_retired) {
        return fail("free physical page has live ownership metadata at page " +
                    std::to_string(physical_page));
      }
    } else {
      if (allocator_free) {
        return fail("owned physical page appears in free allocator at page " +
                    std::to_string(physical_page));
      }
      if ((ownership == kPhysicalRetired) != allocator_retired) {
        return fail("retired physical state/range mismatch at page " +
                    std::to_string(physical_page));
      }
    }

    if (ownership == kPhysicalRetired &&
        physical_mapping_refs_[physical_page] != 0u) {
      return fail("retired physical page still has guest mappings at page " +
                  std::to_string(physical_page));
    }
    if (ownership == kPhysicalAnonymousPendingFree &&
        physical_mapping_refs_[physical_page] == 0u) {
      return fail("pending-free physical page has no remaining alias at page " +
                  std::to_string(physical_page));
    }
    if (ownership == kPhysicalAnonymous &&
        physical_mapping_refs_[physical_page] == 0u) {
      return fail("anonymous physical page has no owning guest mapping at page " +
                  std::to_string(physical_page));
    }

    const auto& metadata = physical_page_metadata_[physical_page];
    if (metadata.explicit_allocation) {
      if (ownership != kPhysicalExplicit ||
          metadata.allocation_base_page == kInvalidPhysicalPage ||
          metadata.allocation_page_count == 0u ||
          metadata.allocation_base_page >= kPhysicalPageCount ||
          std::uint64_t{metadata.allocation_base_page} +
                  metadata.allocation_page_count >
              kPhysicalPageCount ||
          !std::has_single_bit(metadata.allocation_page_size) ||
          metadata.allocation_page_size < kBasePageSize ||
          !valid_memory_type_protection(metadata.current_protect) ||
          !valid_memory_type_protection(metadata.allocation_protect)) {
        return fail("invalid physical allocation metadata at page " +
                    std::to_string(physical_page));
      }
      const auto& base_metadata =
          physical_page_metadata_[metadata.allocation_base_page];
      if (!base_metadata.explicit_allocation ||
          base_metadata.allocation_base_page !=
              metadata.allocation_base_page ||
          base_metadata.allocation_page_count !=
              metadata.allocation_page_count ||
          base_metadata.allocation_page_size !=
              metadata.allocation_page_size ||
          base_metadata.allocation_protect != metadata.allocation_protect) {
        return fail("physical allocation metadata identity mismatch at page " +
                    std::to_string(physical_page));
      }
    } else if (ownership == kPhysicalExplicit) {
      return fail("explicit physical ownership missing allocation metadata at page " +
                  std::to_string(physical_page));
    }
  }

  for (const auto& [physical_page, guest_pages] :
       physical_reverse_mappings_->entries()) {
    if (physical_page >= kPhysicalPageCount) {
      return fail("reverse map contains out-of-range physical page");
    }
    for (std::size_t i = 0; i < guest_pages.size(); ++i) {
      const auto guest_page = guest_pages[i];
      if (guest_page >= kPageCount) {
        return fail("reverse map contains out-of-range guest page");
      }
      for (std::size_t j = i + 1; j < guest_pages.size(); ++j) {
        if (guest_pages[j] == guest_page) {
          return fail("reverse map contains duplicate guest page");
        }
      }
      const auto& page = pages_[guest_page];
      if (page.state != PageState::Committed ||
          page.physical_page != physical_page) {
        return fail("reverse map points at stale guest mapping page " +
                    std::to_string(guest_page));
      }
    }
  }

  for (std::uint32_t guest_page = 0; guest_page < kPageCount; ++guest_page) {
    const auto& page = pages_[guest_page];
    if (page.state == PageState::Committed) {
      if (page.permanent_guard) {
        if (page.physical_page != kInvalidPhysicalPage ||
            page.current_protect != Protect::None) {
          return fail("permanent guard page has backing or access rights at page " +
                      std::to_string(guest_page));
        }
        continue;
      }
      if (page.physical_page == kInvalidPhysicalPage ||
          page.physical_page >= kPhysicalPageCount) {
        return fail("committed guest page has invalid physical backing at page " +
                    std::to_string(guest_page));
      }
      if ((page.kind == RegionKind::Virtual || page.kind == RegionKind::Xex) &&
          !physical_reverse_mappings_->contains(page.physical_page,
                                                guest_page)) {
        return fail("committed guest page missing reverse mapping at page " +
                    std::to_string(guest_page));
      }
    } else if (page.physical_page != kInvalidPhysicalPage) {
      return fail("non-committed guest page retains physical backing at page " +
                  std::to_string(guest_page));
    }

    if (page.explicit_physical_mapping && page.state != PageState::Committed) {
      return fail("non-committed guest page retains explicit-mapping state at page " +
                  std::to_string(guest_page));
    }

    if (page.permanent_guard && page.state != PageState::Committed) {
      return fail("permanent guard page is not committed at page " +
                  std::to_string(guest_page));
    }

    if (page.kind == RegionKind::Xex && page.state != PageState::Free) {
      const auto address = guest_page << kPageShift;
      const auto alias_address =
          address < kXex4KBase ? address + 0x10000000u
                               : address - 0x10000000u;
      const auto alias_page_index = alias_address >> kPageShift;
      if (alias_page_index >= kPageCount) {
        return fail("XEX alias resolves outside guest page table");
      }
      const auto& alias = pages_[alias_page_index];
      if (alias.kind != RegionKind::Xex || alias.state != page.state ||
          alias.allocation_page_count != page.allocation_page_count ||
          alias.allocation_protect != page.allocation_protect ||
          alias.current_protect != page.current_protect ||
          alias.physical_page != page.physical_page) {
        return fail("XEX alias metadata mismatch at page " +
                    std::to_string(guest_page));
      }
    }
  }

  if (error) error->clear();
  return true;
}

}  // namespace xenon::memory
