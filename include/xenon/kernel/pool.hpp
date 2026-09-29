#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace xenon::kernel {

class KernelMemory;

// The kernel pool behind ExAllocatePool*/ExFreePool. Blocks live in Memory V2
// guest virtual memory; no host pointer is ever exposed as a guest address.
//
// Layout follows the observable Xbox 360 contract (verified against the
// reference kernels): an allocation of at most kSmallLimit bytes is carved from a
// shared arena as an 8-byte header followed by the payload, with the block
// 64-byte aligned; the header's third byte is the 0xAA pool marker and its last
// four bytes are the caller's big-endian tag. Larger requests are page-aligned
// stand-alone allocations with no header. ExFreePool derives which form a
// pointer is from Xenon's own bookkeeping, so a pointer the pool never issued
// (or already freed) is rejected rather than corrupting an arena.
//
// Thread-safe.
class KernelPool final {
 public:
  static constexpr std::uint32_t kSmallLimit = 0xFD8u;
  static constexpr std::uint32_t kHeaderSize = 8u;
  static constexpr std::uint32_t kBlockAlignment = 64u;
  static constexpr std::uint32_t kArenaSize = 0x100000u;
  static constexpr std::uint8_t kPoolMarker = 0xAAu;

  explicit KernelPool(KernelMemory& memory) noexcept : memory_(memory) {}
  ~KernelPool() noexcept;

  KernelPool(const KernelPool&) = delete;
  KernelPool& operator=(const KernelPool&) = delete;

  // Returns the guest address of the payload, or 0 if memory is exhausted.
  [[nodiscard]] std::uint32_t allocate(std::uint32_t size, std::uint32_t tag);
  // False if `address` is not a live pool allocation (never issued or already
  // freed). Freeing address 0 succeeds, matching ExFreePool(NULL).
  [[nodiscard]] bool free(std::uint32_t address) noexcept;
  // Usable payload bytes of a live allocation, or 0 if `address` is not one.
  [[nodiscard]] std::uint32_t usable_size(std::uint32_t address) const noexcept;

  void release_all() noexcept;
  [[nodiscard]] std::size_t outstanding_allocations() const noexcept;

 private:
  struct Arena {
    std::uint32_t base{};
    std::map<std::uint32_t, std::uint32_t> free_runs;  // offset -> size
  };
  struct Allocation {
    std::size_t arena{};        // index into arenas_ (small blocks)
    std::uint32_t block_offset{};
    std::uint32_t block_size{};
    std::uint32_t requested{};
    bool large{};
  };

  [[nodiscard]] std::uint32_t allocate_small_locked(std::uint32_t size, std::uint32_t tag);
  [[nodiscard]] std::uint32_t allocate_large_locked(std::uint32_t size);
  [[nodiscard]] bool add_arena_locked();
  void return_run_locked(Arena& arena, std::uint32_t offset, std::uint32_t size);

  KernelMemory& memory_;
  mutable std::mutex mutex_;
  std::vector<Arena> arenas_;
  std::unordered_map<std::uint32_t, Allocation> allocations_;  // by payload address
};

}  // namespace xenon::kernel
