#include "xenon/kernel/pool.hpp"

#include <algorithm>

#include "xenon/kernel/memory.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/types.hpp"

namespace xenon::kernel {
namespace {

constexpr std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

}  // namespace

KernelPool::~KernelPool() noexcept {
  release_all();
}

bool KernelPool::add_arena_locked() {
  std::uint32_t base = 0;
  if (!memory_.allocate_virtual(base, kArenaSize, memory::kReadWrite, /*top_down=*/false,
                                /*zero_initialize=*/true)) {
    return false;
  }
  Arena arena;
  arena.base = base;
  arena.free_runs.emplace(0u, kArenaSize);
  arenas_.push_back(std::move(arena));
  return true;
}

std::uint32_t KernelPool::allocate_small_locked(std::uint32_t size, std::uint32_t tag) {
  const std::uint32_t need = align_up(size + kHeaderSize, kBlockAlignment);
  for (int attempt = 0; attempt < 2; ++attempt) {
    for (std::size_t index = 0; index < arenas_.size(); ++index) {
      auto& arena = arenas_[index];
      for (auto it = arena.free_runs.begin(); it != arena.free_runs.end(); ++it) {
        if (it->second < need) continue;
        const std::uint32_t offset = it->first;
        const std::uint32_t run_size = it->second;
        arena.free_runs.erase(it);
        if (run_size > need) arena.free_runs.emplace(offset + need, run_size - need);

        const std::uint32_t block = arena.base + offset;
        auto& space = memory_.address_space();
        // Pool header: byte 2 is the 0xAA marker, the last word the tag.
        space.write32_be(block, 0u);
        space.write8(block + 2u, kPoolMarker);
        space.write32_be(block + 4u, tag);
        const std::uint32_t payload = block + kHeaderSize;
        allocations_[payload] = Allocation{index, offset, need, size, false};
        return payload;
      }
    }
    if (attempt == 0 && !add_arena_locked()) return 0;
  }
  return 0;
}

std::uint32_t KernelPool::allocate_large_locked(std::uint32_t size) {
  std::uint32_t base = 0;
  if (!memory_.allocate_virtual(base, size, memory::kReadWrite, /*top_down=*/false,
                                /*zero_initialize=*/true)) {
    return 0;
  }
  allocations_[base] = Allocation{0, 0, size, size, true};
  return base;
}

std::uint32_t KernelPool::allocate(std::uint32_t size, std::uint32_t tag) {
  std::scoped_lock lock(mutex_);
  return size <= kSmallLimit ? allocate_small_locked(size, tag) : allocate_large_locked(size);
}

void KernelPool::return_run_locked(Arena& arena, std::uint32_t offset, std::uint32_t size) {
  auto next = arena.free_runs.lower_bound(offset);
  // Merge with the run that ends where this block starts.
  if (next != arena.free_runs.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == offset) {
      offset = prev->first;
      size += prev->second;
      arena.free_runs.erase(prev);
    }
  }
  // Merge with the run that starts where this block ends.
  next = arena.free_runs.lower_bound(offset + size);
  if (next != arena.free_runs.end() && next->first == offset + size) {
    size += next->second;
    arena.free_runs.erase(next);
  }
  arena.free_runs.emplace(offset, size);
}

bool KernelPool::free(std::uint32_t address) noexcept {
  if (address == 0u) return true;
  std::scoped_lock lock(mutex_);
  const auto it = allocations_.find(address);
  if (it == allocations_.end()) return false;
  const auto allocation = it->second;
  if (allocation.large) {
    if (!memory_.free_virtual(address)) return false;
  } else {
    // Clear the marker so a stale pointer's header no longer reads as live.
    memory_.address_space().write8(address - kHeaderSize + 2u, 0u);
    return_run_locked(arenas_[allocation.arena], allocation.block_offset, allocation.block_size);
  }
  allocations_.erase(it);
  return true;
}

std::uint32_t KernelPool::usable_size(std::uint32_t address) const noexcept {
  std::scoped_lock lock(mutex_);
  const auto it = allocations_.find(address);
  if (it == allocations_.end()) return 0u;
  return it->second.large ? it->second.requested : it->second.block_size - kHeaderSize;
}

void KernelPool::release_all() noexcept {
  std::scoped_lock lock(mutex_);
  for (const auto& [address, allocation] : allocations_) {
    if (allocation.large) static_cast<void>(memory_.free_virtual(address));
  }
  for (const auto& arena : arenas_) static_cast<void>(memory_.free_virtual(arena.base));
  allocations_.clear();
  arenas_.clear();
}

std::size_t KernelPool::outstanding_allocations() const noexcept {
  std::scoped_lock lock(mutex_);
  return allocations_.size();
}

}  // namespace xenon::kernel
