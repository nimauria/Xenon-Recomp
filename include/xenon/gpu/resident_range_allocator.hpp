#pragma once

// Sub-allocation of a persistent GPU buffer whose ranges outlive single
// submissions: a range released while the GPU may still read it becomes
// reusable only once the queue has completed past the value it was released
// at. Used for GPU-resident processed indices (index_cache.hpp): a converted
// index buffer is copied into the resident buffer once and drawn from there
// until the cache drops it, instead of being copied into the transient upload
// arena by every draw.
//
// First fit over free ranges kept coalesced; not thread-safe (one backend's
// draw consumption owns it).

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace xenon::gpu {

class ResidentRangeAllocator {
 public:
  explicit ResidentRangeAllocator(std::uint64_t capacity = 0u) { reset(capacity); }

  void reset(std::uint64_t capacity);

  // `size` bytes at a multiple of `alignment` (a power of two), or null when
  // no free range holds them (ranges still pending retirement do not count).
  [[nodiscard]] std::optional<std::uint64_t> allocate(std::uint64_t size,
                                                      std::uint64_t alignment);
  // Gives back [offset, offset + size) once the queue completes `retire_after`.
  void release(std::uint64_t offset, std::uint64_t size, std::uint64_t retire_after);
  // Makes every range released at or before `completed` free again.
  void reclaim(std::uint64_t completed);

  [[nodiscard]] std::uint64_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::uint64_t free_bytes() const noexcept { return free_bytes_; }
  [[nodiscard]] std::uint64_t pending_bytes() const noexcept { return pending_bytes_; }

 private:
  void insert_free(std::uint64_t offset, std::uint64_t size);
  struct Pending {
    std::uint64_t offset{};
    std::uint64_t size{};
    std::uint64_t retire_after{};
  };
  std::uint64_t capacity_{};
  std::uint64_t free_bytes_{};
  std::uint64_t pending_bytes_{};
  std::map<std::uint64_t, std::uint64_t> free_;  // offset -> size, coalesced
  std::vector<Pending> pending_;
};

}  // namespace xenon::gpu
