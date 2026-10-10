#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

namespace xenon::kernel {

class KernelMemory;

// Process-owned Xbox guest heap service used by native RTL heap replacements.
// Allocations always live in the existing Memory V2 guest virtual address
// space; no host pointer is ever exposed as a guest address.
//
// Like the real RTL heap, small requests are sub-allocated out of larger
// committed segments (kSegmentSize) owned by one heap handle, so a title can
// keep hundreds of thousands of small CRT blocks live at once. Only requests
// above kLargeThreshold get a dedicated virtual allocation each (the RTL
// heap's "VirtualAlloc'd block" path). Blocks never cross segments, and
// segments are never shared between heap handles.
class GuestHeapManager final {
 public:
  static constexpr std::uint32_t kHeapZeroMemory = 0x00000008u;
  static constexpr std::uint32_t kInvalidSize = 0xFFFFFFFFu;
  // Every returned block is aligned to this (sufficient for VMX128 lvx/stvx
  // on heap data, which titles rely on).
  static constexpr std::uint32_t kBlockAlignment = 16u;
  static constexpr std::uint32_t kSegmentSize = 1024u * 1024u;
  static constexpr std::uint32_t kLargeThreshold = 256u * 1024u;
  // Identity of the XAM-owned heap (XamAlloc/XamFree), kept apart from every
  // title RTL heap. Title heap handles are guest addresses of heap headers, so
  // a value inside the never-mapped null page cannot collide with one; 0 and 1
  // are already the default-process-heap spellings.
  static constexpr std::uint32_t kXamHeapHandle = 2u;

  explicit GuestHeapManager(KernelMemory& memory) noexcept : memory_(memory) {}
  ~GuestHeapManager() noexcept;

  GuestHeapManager(const GuestHeapManager&) = delete;
  GuestHeapManager& operator=(const GuestHeapManager&) = delete;

  [[nodiscard]] std::uint32_t allocate(std::uint32_t heap_handle,
                                       std::uint32_t flags,
                                       std::uint32_t size);
  [[nodiscard]] bool free(std::uint32_t heap_handle, std::uint32_t flags,
                          std::uint32_t address) noexcept;
  [[nodiscard]] std::uint32_t size(std::uint32_t heap_handle,
                                   std::uint32_t flags,
                                   std::uint32_t address) const noexcept;
  [[nodiscard]] std::uint32_t reallocate(std::uint32_t heap_handle,
                                         std::uint32_t flags,
                                         std::uint32_t address,
                                         std::uint32_t new_size);

  void release_all() noexcept;

  [[nodiscard]] std::size_t outstanding_allocations() const noexcept;
  [[nodiscard]] bool owns(std::uint32_t heap_handle,
                          std::uint32_t address) const noexcept;
  // Number of committed sub-allocation segments across all heaps (excludes
  // dedicated large-block allocations). Exposed for tests.
  [[nodiscard]] std::size_t segment_count() const noexcept;

 private:
  struct Allocation {
    std::uint32_t heap_handle{};
    std::uint32_t requested_size{};
    // Bytes reserved for this block: the 16-byte-rounded size for a
    // segment block, or the whole virtual allocation for a large block.
    std::uint32_t block_size{};
    // Base of the owning segment; 0 for a dedicated large-block allocation.
    std::uint32_t segment_base{};
  };

  struct Segment {
    std::uint32_t size{};
    std::uint32_t used_bytes{};
  };

  struct FreeRun {
    std::uint32_t size{};
    std::uint32_t segment_base{};
  };

  struct Heap {
    std::map<std::uint32_t, Segment> segments;  // keyed by base address
    std::map<std::uint32_t, FreeRun> runs_by_address;
    // (size, address) - lower_bound gives the best (smallest sufficient) fit.
    std::set<std::pair<std::uint32_t, std::uint32_t>> runs_by_size;
  };

  [[nodiscard]] static std::uint32_t normalize_heap_handle(
      std::uint32_t heap_handle) noexcept;
  [[nodiscard]] std::uint32_t allocate_locked(std::uint32_t heap_handle,
                                              std::uint32_t flags,
                                              std::uint32_t size);
  [[nodiscard]] std::uint32_t allocate_large_locked(std::uint32_t heap_handle,
                                                    std::uint32_t flags,
                                                    std::uint32_t size);
  [[nodiscard]] std::uint32_t allocate_block_locked(Heap& heap,
                                                    std::uint32_t heap_handle,
                                                    std::uint32_t flags,
                                                    std::uint32_t size);
  [[nodiscard]] bool add_segment_locked(Heap& heap);
  [[nodiscard]] bool free_locked(std::uint32_t address) noexcept;
  void insert_run_locked(Heap& heap, std::uint32_t address, FreeRun run);
  void erase_run_locked(Heap& heap,
                        std::map<std::uint32_t, FreeRun>::iterator it);

  KernelMemory& memory_;
  mutable std::mutex mutex_;
  std::unordered_map<std::uint32_t, Allocation> allocations_;
  std::unordered_map<std::uint32_t, Heap> heaps_;
};

}  // namespace xenon::kernel
