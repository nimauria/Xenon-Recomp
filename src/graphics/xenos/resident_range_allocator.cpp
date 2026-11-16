#include "xenon/gpu/resident_range_allocator.hpp"

#include <algorithm>
#include <bit>

namespace xenon::gpu {

void ResidentRangeAllocator::reset(std::uint64_t capacity, std::uint64_t granule) {
  granule_ = std::has_single_bit(granule) ? granule : kDefaultGranule;
  capacity_ = capacity & ~(granule_ - 1u);
  by_offset_.clear();
  by_size_.clear();
  pending_.clear();
  pending_bytes_ = 0u;
  free_bytes_ = 0u;
  if (capacity_) insert_free(0u, capacity_);
}

std::optional<std::uint64_t> ResidentRangeAllocator::allocate(std::uint64_t size) {
  if (!size || size > capacity_) return std::nullopt;
  const auto needed = rounded(size);
  // Best fit: the smallest free range that holds the request. Every range
  // starts on a granule, so the first one found fits.
  const auto candidate = by_size_.lower_bound(needed);
  if (candidate == by_size_.end()) return std::nullopt;
  const auto offset = candidate->second;
  const auto range = by_offset_.find(offset);
  const auto available = range->second;
  erase_free(range);
  if (available > needed) insert_free(offset + needed, available - needed);
  return offset;
}

void ResidentRangeAllocator::release(std::uint64_t offset, std::uint64_t size,
                                     std::uint64_t retire_after) {
  if (!size) return;
  const auto bytes = rounded(size);
  pending_.push_back({offset, bytes, retire_after});
  pending_bytes_ += bytes;
}

void ResidentRangeAllocator::reclaim(std::uint64_t completed) {
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->retire_after > completed) {
      ++it;
      continue;
    }
    pending_bytes_ -= it->size;
    insert_free(it->offset, it->size);
    it = pending_.erase(it);
  }
}

void ResidentRangeAllocator::erase_free(std::map<std::uint64_t, std::uint64_t>::iterator range) {
  const auto [first, last] = by_size_.equal_range(range->second);
  for (auto it = first; it != last; ++it) {
    if (it->second == range->first) {
      by_size_.erase(it);
      break;
    }
  }
  free_bytes_ -= range->second;
  by_offset_.erase(range);
}

void ResidentRangeAllocator::insert_free(std::uint64_t offset, std::uint64_t size) {
  auto end = offset + size;
  // Merge with the following range...
  if (const auto next = by_offset_.find(end); next != by_offset_.end()) {
    end += next->second;
    erase_free(next);
  }
  // ...and the preceding one.
  auto after = by_offset_.lower_bound(offset);
  if (after != by_offset_.begin()) {
    const auto before = std::prev(after);
    if (before->first + before->second == offset) {
      offset = before->first;
      erase_free(before);
    }
  }
  by_offset_.emplace(offset, end - offset);
  by_size_.emplace(end - offset, offset);
  free_bytes_ += end - offset;
}

}  // namespace xenon::gpu
