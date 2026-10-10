#pragma once

// Physical page range allocation and physical-to-guest reverse mappings.
// Private to xenon_memory; included through address_space_internal.hpp.

#include "xenon/logging/probe_log.hpp"

namespace xenon::memory {

class AddressSpace::PhysicalRangeAllocator {
 public:
  void reset(std::uint32_t first_page, std::uint32_t page_count) {
    free_ranges_.clear();
    retired_ranges_.clear();
    if (page_count) free_ranges_.emplace(first_page, page_count);
  }

  [[nodiscard]] bool allocate(std::uint32_t page_count,
                              std::uint32_t alignment_pages, bool top_down,
                              std::uint32_t& out_first_page,
                              std::uint32_t readers_snapshot = 0u) {
    return allocate(page_count, alignment_pages, top_down, 0u,
                    kPhysicalPageCount, out_first_page, readers_snapshot);
  }

  [[nodiscard]] bool allocate(std::uint32_t page_count,
                              std::uint32_t alignment_pages, bool top_down,
                              std::uint32_t minimum_page,
                              std::uint32_t maximum_page_exclusive,
                              std::uint32_t& out_first_page,
                              std::uint32_t readers_snapshot = 0u) {
    if (!page_count || !alignment_pages ||
        !std::has_single_bit(alignment_pages) ||
        minimum_page >= maximum_page_exclusive ||
        maximum_page_exclusive > kPhysicalPageCount) {
      return false;
    }

    if (!top_down) {
      for (auto it = free_ranges_.begin(); it != free_ranges_.end(); ++it) {
        const auto range_first = std::max(it->first, minimum_page);
        const auto range_end = std::min<std::uint64_t>(
            std::uint64_t(it->first) + it->second,
            maximum_page_exclusive);
        if (range_first >= range_end) continue;
        const auto candidate = align_page_up(range_first, alignment_pages);
        if (std::uint64_t(candidate) + page_count > range_end) continue;
        consume(it, candidate, page_count);
        out_first_page = candidate;
        return true;
      }
      diag_log_exhaustion(page_count, readers_snapshot);
      return false;
    }

    for (auto rit = free_ranges_.rbegin(); rit != free_ranges_.rend(); ++rit) {
      const auto range_first = std::max(rit->first, minimum_page);
      const auto range_end = std::min<std::uint64_t>(
          std::uint64_t(rit->first) + rit->second,
          maximum_page_exclusive);
      if (range_first >= range_end || range_end - range_first < page_count) {
        continue;
      }
      const auto latest = static_cast<std::uint32_t>(range_end - page_count);
      const auto candidate = align_page_down(latest, alignment_pages);
      if (candidate < range_first) continue;
      auto it = std::prev(rit.base());
      consume(it, candidate, page_count);
      out_first_page = candidate;
      return true;
    }
    diag_log_exhaustion(page_count, readers_snapshot);
    return false;
  }

  void diag_log_exhaustion(std::uint32_t requested_page_count,
                           std::uint32_t readers_snapshot) const {
    std::uint64_t free_total = 0;
    for (const auto& [first, count] : free_ranges_) free_total += count;
    std::uint64_t retired_total = 0;
    for (const auto& [first, count] : retired_ranges_) retired_total += count;
    xenon::logging::append_probe_log(
        "phys_alloc_fail_diag.log",
        "PHYS_ALLOC_FAIL requested=%u free_total=%llu free_ranges=%zu "
        "retired_total=%llu retired_ranges=%zu active_fast_readers=%u\n",
        requested_page_count, static_cast<unsigned long long>(free_total), free_ranges_.size(),
        static_cast<unsigned long long>(retired_total), retired_ranges_.size(), readers_snapshot);
  }

  [[nodiscard]] bool contains_free(std::uint32_t first_page,
                                   std::uint32_t page_count) const {
    return contains(free_ranges_, first_page, page_count);
  }

  [[nodiscard]] bool contains_retired(std::uint32_t first_page,
                                      std::uint32_t page_count) const {
    return contains(retired_ranges_, first_page, page_count);
  }

  [[nodiscard]] bool claim(std::uint32_t first_page,
                           std::uint32_t page_count) {
    if (!contains_free(first_page, page_count)) return false;
    auto it = free_ranges_.upper_bound(first_page);
    --it;
    consume(it, first_page, page_count);
    return true;
  }

  [[nodiscard]] bool release(std::uint32_t first_page,
                             std::uint32_t page_count) {
    return insert_coalesced(free_ranges_, first_page, page_count);
  }

  [[nodiscard]] bool retire(std::uint32_t first_page,
                            std::uint32_t page_count) {
    return insert_coalesced(retired_ranges_, first_page, page_count);
  }

  // Cheap peek used to skip the reclaim grace-period latch entirely when
  // there is nothing retired to reclaim - the overwhelmingly common case,
  // since this is checked at the start of every physical page allocation.
  [[nodiscard]] bool has_retired_ranges() const noexcept {
    return !retired_ranges_.empty();
  }

  [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
  take_retired_ranges() {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    ranges.reserve(retired_ranges_.size());
    for (const auto& range : retired_ranges_) ranges.push_back(range);
    retired_ranges_.clear();
    return ranges;
  }

 private:
  using RangeMap = std::map<std::uint32_t, std::uint32_t>;

  static bool contains(const RangeMap& ranges, std::uint32_t first_page,
                       std::uint32_t page_count) {
    if (!page_count) return false;
    auto it = ranges.upper_bound(first_page);
    if (it == ranges.begin()) return false;
    --it;
    const auto range_end = std::uint64_t(it->first) + it->second;
    return first_page >= it->first &&
           std::uint64_t(first_page) + page_count <= range_end;
  }

  static bool insert_coalesced(RangeMap& ranges, std::uint32_t first_page,
                               std::uint32_t page_count) {
    if (!page_count) return false;
    const auto end_page = std::uint64_t(first_page) + page_count;
    if (end_page > kPhysicalPageCount) return false;

    auto next = ranges.lower_bound(first_page);
    if (next != ranges.end() && end_page > next->first) return false;

    auto prev = next;
    if (prev != ranges.begin()) {
      --prev;
      const auto prev_end = std::uint64_t(prev->first) + prev->second;
      if (prev_end > first_page) return false;
    } else {
      prev = ranges.end();
    }

    std::uint32_t merged_first = first_page;
    std::uint32_t merged_count = page_count;
    if (prev != ranges.end() &&
        std::uint64_t(prev->first) + prev->second == first_page) {
      merged_first = prev->first;
      merged_count += prev->second;
      ranges.erase(prev);
    }

    next = ranges.lower_bound(merged_first);
    if (next != ranges.end() &&
        std::uint64_t(merged_first) + merged_count == next->first) {
      merged_count += next->second;
      ranges.erase(next);
    }

    ranges.emplace(merged_first, merged_count);
    return true;
  }

  static std::uint32_t align_page_up(std::uint32_t value,
                                     std::uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
  }

  static std::uint32_t align_page_down(std::uint32_t value,
                                       std::uint32_t alignment) {
    return value & ~(alignment - 1u);
  }

  void consume(RangeMap::iterator it, std::uint32_t first_page,
               std::uint32_t page_count) {
    const auto range_first = it->first;
    const auto range_count = it->second;
    const auto range_end = range_first + range_count;
    const auto allocation_end = first_page + page_count;
    free_ranges_.erase(it);
    if (range_first < first_page) {
      free_ranges_.emplace(range_first, first_page - range_first);
    }
    if (allocation_end < range_end) {
      free_ranges_.emplace(allocation_end, range_end - allocation_end);
    }
  }

  RangeMap free_ranges_{};
  RangeMap retired_ranges_{};
};

class AddressSpace::PhysicalReverseMappings {
 public:
  void clear() { mappings_.clear(); }

  [[nodiscard]] bool add(std::uint32_t physical_page,
                         std::uint32_t guest_page) {
    auto& guests = mappings_[physical_page];
    if (std::find(guests.begin(), guests.end(), guest_page) != guests.end()) {
      return false;
    }
    guests.push_back(guest_page);
    return true;
  }

  [[nodiscard]] bool remove(std::uint32_t physical_page,
                            std::uint32_t guest_page) {
    const auto it = mappings_.find(physical_page);
    if (it == mappings_.end()) return false;
    auto& guests = it->second;
    const auto guest_it = std::find(guests.begin(), guests.end(), guest_page);
    if (guest_it == guests.end()) return false;
    *guest_it = guests.back();
    guests.pop_back();
    if (guests.empty()) mappings_.erase(it);
    return true;
  }

  [[nodiscard]] std::size_t count(std::uint32_t physical_page) const {
    const auto it = mappings_.find(physical_page);
    return it == mappings_.end() ? 0u : it->second.size();
  }

  [[nodiscard]] bool contains(std::uint32_t physical_page,
                              std::uint32_t guest_page) const {
    const auto it = mappings_.find(physical_page);
    if (it == mappings_.end()) return false;
    const auto& guests = it->second;
    return std::find(guests.begin(), guests.end(), guest_page) != guests.end();
  }

  [[nodiscard]] std::vector<std::uint32_t> guests(
      std::uint32_t physical_page) const {
    const auto it = mappings_.find(physical_page);
    return it == mappings_.end() ? std::vector<std::uint32_t>{} : it->second;
  }

  using Map = std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>;
  [[nodiscard]] const Map& entries() const noexcept { return mappings_; }

 private:
  Map mappings_{};
};

}  // namespace xenon::memory
