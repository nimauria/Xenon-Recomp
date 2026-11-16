#include "xenon/gpu/resident_range_allocator.hpp"

#include <algorithm>
#include <bit>

namespace xenon::gpu {

void ResidentRangeAllocator::reset(std::uint64_t capacity) {
  capacity_ = capacity;
  free_.clear();
  pending_.clear();
  pending_bytes_ = 0u;
  free_bytes_ = 0u;
  if (capacity) insert_free(0u, capacity);
}

std::optional<std::uint64_t> ResidentRangeAllocator::allocate(std::uint64_t size,
                                                              std::uint64_t alignment) {
  if (!size || !std::has_single_bit(alignment)) return std::nullopt;
  for (auto range = free_.begin(); range != free_.end(); ++range) {
    const auto start = range->first;
    const auto end = start + range->second;
    const auto aligned = (start + alignment - 1u) & ~(alignment - 1u);
    if (aligned + size > end || aligned + size < aligned) continue;
    free_.erase(range);
    free_bytes_ -= end - start;
    if (aligned > start) insert_free(start, aligned - start);
    if (aligned + size < end) insert_free(aligned + size, end - (aligned + size));
    return aligned;
  }
  return std::nullopt;
}

void ResidentRangeAllocator::release(std::uint64_t offset, std::uint64_t size,
                                     std::uint64_t retire_after) {
  if (!size) return;
  pending_.push_back({offset, size, retire_after});
  pending_bytes_ += size;
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

void ResidentRangeAllocator::insert_free(std::uint64_t offset, std::uint64_t size) {
  auto end = offset + size;
  free_bytes_ += size;
  // Merge with the following range...
  if (const auto next = free_.find(end); next != free_.end()) {
    end += next->second;
    free_.erase(next);
  }
  // ...and the preceding one.
  auto after = free_.lower_bound(offset);
  if (after != free_.begin()) {
    const auto before = std::prev(after);
    if (before->first + before->second == offset) {
      before->second = end - before->first;
      return;
    }
  }
  free_.emplace(offset, end - offset);
}

}  // namespace xenon::gpu
