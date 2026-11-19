#pragma once

// Sub-allocation of a persistent GPU buffer whose ranges outlive single
// submissions: a range released while the GPU may still read it becomes
// reusable only once the queue has completed past the value it was released
// at. Used for GPU-resident processed indices (index_cache.hpp): a converted
// index buffer is copied into the resident buffer once and drawn from there
// until the cache drops it, instead of being copied into the transient upload
// arena by every draw.
//
// Sizes are rounded up to `granule` bytes (a power of two), so every range
// starts on a granule boundary and the first free range at least as large as
// a request always fits it: allocation is a best-fit lookup in a size-ordered
// index, independent of how fragmented the buffer is (a first-fit scan over a
// mission's fragmented free list cost ~20 us an allocation). Free ranges stay
// coalesced. Not thread-safe (one backend's draw consumption owns it).

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace xenon::gpu {

class ResidentRangeAllocator {
 public:
  static constexpr std::uint64_t kDefaultGranule = 256;

  explicit ResidentRangeAllocator(std::uint64_t capacity = 0u,
                                  std::uint64_t granule = kDefaultGranule) {
    reset(capacity, granule);
  }

  // Capacity is rounded down to a whole number of granules.
  void reset(std::uint64_t capacity, std::uint64_t granule = kDefaultGranule);

  // `size` bytes (rounded up to the granule) on a granule boundary, or null
  // when no free range holds them (ranges still pending retirement do not
  // count).
  [[nodiscard]] std::optional<std::uint64_t> allocate(std::uint64_t size);
  // Gives back the range allocate() returned for `size`, once the queue
  // completes `retire_after`.
  void release(std::uint64_t offset, std::uint64_t size, std::uint64_t retire_after);
  // Makes every range released at or before `completed` free again.
  void reclaim(std::uint64_t completed);

  [[nodiscard]] std::uint64_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::uint64_t granule() const noexcept { return granule_; }
  [[nodiscard]] std::uint64_t free_bytes() const noexcept { return free_bytes_; }
  [[nodiscard]] std::uint64_t pending_bytes() const noexcept { return pending_bytes_; }
  [[nodiscard]] std::size_t free_ranges() const noexcept { return by_offset_.size(); }

 private:
  [[nodiscard]] std::uint64_t rounded(std::uint64_t size) const noexcept {
    return (size + granule_ - 1u) & ~(granule_ - 1u);
  }
  void insert_free(std::uint64_t offset, std::uint64_t size);
  void erase_free(std::map<std::uint64_t, std::uint64_t>::iterator range);
  struct Pending {
    std::uint64_t offset{};
    std::uint64_t size{};
    std::uint64_t retire_after{};
  };
  std::uint64_t capacity_{};
  std::uint64_t granule_{kDefaultGranule};
  std::uint64_t free_bytes_{};
  std::uint64_t pending_bytes_{};
  std::map<std::uint64_t, std::uint64_t> by_offset_;  // offset -> size, coalesced
  std::multimap<std::uint64_t, std::uint64_t> by_size_;  // size -> offset
  std::vector<Pending> pending_;
};

}  // namespace xenon::gpu
