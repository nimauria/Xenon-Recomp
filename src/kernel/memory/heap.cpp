#include "xenon/kernel/heap.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>
#include <vector>

#include "xenon/kernel/memory.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/types.hpp"

namespace xenon::kernel {
namespace {

constexpr std::uint32_t kDefaultProcessHeapIdentity = 1u;

constexpr std::uint32_t align_block(std::uint32_t size) noexcept {
  return (size + GuestHeapManager::kBlockAlignment - 1u) &
         ~(GuestHeapManager::kBlockAlignment - 1u);
}

}  // namespace

GuestHeapManager::~GuestHeapManager() noexcept {
  release_all();
}

std::uint32_t GuestHeapManager::normalize_heap_handle(
    std::uint32_t heap_handle) noexcept {
  // Some XDK CRT call paths pass a null/default process-heap token while
  // others carry an opaque non-zero RTL heap handle. Preserve non-zero handle
  // identity exactly; normalize only the null/default spelling so allocations
  // cannot accidentally cross independently identified heaps.
  return heap_handle ? heap_handle : kDefaultProcessHeapIdentity;
}

std::uint32_t GuestHeapManager::allocate(std::uint32_t heap_handle,
                                         std::uint32_t flags,
                                         std::uint32_t size) {
  std::scoped_lock lock(mutex_);
  return allocate_locked(normalize_heap_handle(heap_handle), flags, size);
}

std::uint32_t GuestHeapManager::allocate_locked(std::uint32_t heap_handle,
                                                std::uint32_t flags,
                                                std::uint32_t size) {
  // Xbox/ReXCRT normalizes a zero-byte request to a one-byte allocation.
  const auto requested = (std::max)(size, 1u);
  if (requested > kLargeThreshold) {
    return allocate_large_locked(heap_handle, flags, requested);
  }
  return allocate_block_locked(heaps_[heap_handle], heap_handle, flags, requested);
}

std::uint32_t GuestHeapManager::allocate_large_locked(std::uint32_t heap_handle,
                                                      std::uint32_t flags,
                                                      std::uint32_t size) {
  std::uint32_t address = 0;
  const bool zero_initialize = (flags & kHeapZeroMemory) != 0u;
  // Memory V2 owns reservation, commitment, permissions, coherency and
  // lifetime; its 64 KiB allocation granularity satisfies block alignment.
  if (!memory_.allocate_virtual(address, size, memory::kReadWrite, false,
                                zero_initialize)) {
    return 0;
  }
  memory::MappingInfo mapping{};
  if (!memory_.query_virtual(address, mapping)) {
    static_cast<void>(memory_.free_virtual(address));
    return 0;
  }
  allocations_.emplace(address, Allocation{heap_handle, size,
                                           static_cast<std::uint32_t>(mapping.allocation_size),
                                           0u});
  return address;
}

bool GuestHeapManager::add_segment_locked(Heap& heap) {
  std::uint32_t base = 0;
  if (!memory_.allocate_virtual(base, kSegmentSize, memory::kReadWrite, false,
                                /*zero_initialize=*/true)) {
    return false;
  }
  heap.segments.emplace(base, Segment{kSegmentSize, 0u});
  insert_run_locked(heap, base, FreeRun{kSegmentSize, base});
  return true;
}

std::uint32_t GuestHeapManager::allocate_block_locked(Heap& heap,
                                                      std::uint32_t heap_handle,
                                                      std::uint32_t flags,
                                                      std::uint32_t size) {
  const auto need = align_block(size);
  auto fit = heap.runs_by_size.lower_bound({need, 0u});
  if (fit == heap.runs_by_size.end()) {
    if (!add_segment_locked(heap)) return 0;
    fit = heap.runs_by_size.lower_bound({need, 0u});
    if (fit == heap.runs_by_size.end()) return 0;
  }

  const auto address = fit->second;
  const auto run_it = heap.runs_by_address.find(address);
  const auto run = run_it->second;
  erase_run_locked(heap, run_it);
  if (run.size > need) {
    insert_run_locked(heap, address + need, FreeRun{run.size - need, run.segment_base});
  }
  heap.segments[run.segment_base].used_bytes += need;

  // Fresh segments are committed zeroed, but a recycled block keeps whatever
  // its previous owner left, exactly like the real RTL heap; only
  // HEAP_ZERO_MEMORY guarantees zeroed contents.
  if ((flags & kHeapZeroMemory) != 0u) {
    memory_.address_space().fill_bytes(address, need, 0);
  }
  allocations_.emplace(address, Allocation{heap_handle, size, need, run.segment_base});
  return address;
}

void GuestHeapManager::insert_run_locked(Heap& heap, std::uint32_t address, FreeRun run) {
  // Coalesce with the neighbouring free runs of the same segment.
  auto next = heap.runs_by_address.lower_bound(address);
  if (next != heap.runs_by_address.begin()) {
    auto prev = std::prev(next);
    if (prev->second.segment_base == run.segment_base &&
        prev->first + prev->second.size == address) {
      address = prev->first;
      run.size += prev->second.size;
      erase_run_locked(heap, prev);
    }
  }
  next = heap.runs_by_address.find(address + run.size);
  if (next != heap.runs_by_address.end() && next->second.segment_base == run.segment_base) {
    run.size += next->second.size;
    erase_run_locked(heap, next);
  }
  heap.runs_by_address.emplace(address, run);
  heap.runs_by_size.emplace(run.size, address);
}

void GuestHeapManager::erase_run_locked(Heap& heap,
                                        std::map<std::uint32_t, FreeRun>::iterator it) {
  heap.runs_by_size.erase({it->second.size, it->first});
  heap.runs_by_address.erase(it);
}

bool GuestHeapManager::free_locked(std::uint32_t address) noexcept {
  const auto it = allocations_.find(address);
  if (it == allocations_.end()) return false;
  const auto allocation = it->second;

  if (allocation.segment_base == 0u) {
    if (!memory_.free_virtual(address)) return false;
    allocations_.erase(it);
    return true;
  }

  auto& heap = heaps_[allocation.heap_handle];
  auto& segment = heap.segments[allocation.segment_base];
  allocations_.erase(it);
  insert_run_locked(heap, address, FreeRun{allocation.block_size, allocation.segment_base});
  segment.used_bytes -= allocation.block_size;

  // Return a fully free segment to the virtual allocator, keeping one per
  // heap so an alloc/free cycle on an otherwise empty heap does not thrash.
  if (segment.used_bytes == 0u && heap.segments.size() > 1u) {
    const auto run_it = heap.runs_by_address.find(allocation.segment_base);
    if (run_it != heap.runs_by_address.end() && run_it->second.size == segment.size &&
        memory_.free_virtual(allocation.segment_base)) {
      erase_run_locked(heap, run_it);
      heap.segments.erase(allocation.segment_base);
    }
  }
  return true;
}

bool GuestHeapManager::free(std::uint32_t heap_handle, std::uint32_t,
                            std::uint32_t address) noexcept {
  if (!address) return true;
  const auto normalized = normalize_heap_handle(heap_handle);
  std::scoped_lock lock(mutex_);
  const auto it = allocations_.find(address);
  // Both an invalid pointer and a second free are rejected instead of being
  // silently accepted.
  if (it == allocations_.end() || it->second.heap_handle != normalized) return false;
  return free_locked(address);
}

std::uint32_t GuestHeapManager::size(std::uint32_t heap_handle, std::uint32_t,
                                     std::uint32_t address) const noexcept {
  if (!address) return kInvalidSize;
  const auto normalized = normalize_heap_handle(heap_handle);
  std::scoped_lock lock(mutex_);
  const auto it = allocations_.find(address);
  if (it == allocations_.end() || it->second.heap_handle != normalized)
    return kInvalidSize;
  return it->second.requested_size;
}

std::uint32_t GuestHeapManager::reallocate(std::uint32_t heap_handle,
                                           std::uint32_t flags,
                                           std::uint32_t address,
                                           std::uint32_t new_size) {
  const auto normalized = normalize_heap_handle(heap_handle);
  std::scoped_lock lock(mutex_);

  if (!address) return allocate_locked(normalized, flags, new_size);

  const auto old_it = allocations_.find(address);
  if (old_it == allocations_.end() || old_it->second.heap_handle != normalized)
    return 0;

  const auto old = old_it->second;
  const auto requested = (std::max)(new_size, 1u);
  const bool zero = (flags & kHeapZeroMemory) != 0u;
  auto zero_growth = [&](std::uint32_t block) {
    if (zero && requested > old.requested_size) {
      memory_.address_space().fill_bytes(block + old.requested_size,
                                         requested - old.requested_size, 0);
    }
  };

  if (old.segment_base == 0u) {
    // Dedicated large block: keep the mapping while the committed allocation
    // still covers the request (shrink/small-grow preserves the address).
    if (requested <= old.block_size) {
      old_it->second.requested_size = requested;
      zero_growth(address);
      return address;
    }
  } else if (requested <= kLargeThreshold) {
    auto& heap = heaps_[normalized];
    auto& segment = heap.segments[old.segment_base];
    const auto need = align_block(requested);
    if (need <= old.block_size) {
      // Shrink in place, returning the tail to the segment.
      if (need < old.block_size) {
        insert_run_locked(heap, address + need,
                          FreeRun{old.block_size - need, old.segment_base});
        segment.used_bytes -= old.block_size - need;
        old_it->second.block_size = need;
      }
      old_it->second.requested_size = requested;
      zero_growth(address);
      return address;
    }
    // Grow in place into an adjacent free run of the same segment.
    const auto extra = need - old.block_size;
    const auto next = heap.runs_by_address.find(address + old.block_size);
    if (next != heap.runs_by_address.end() && next->second.segment_base == old.segment_base &&
        next->second.size >= extra) {
      const auto run = next->second;
      erase_run_locked(heap, next);
      if (run.size > extra) {
        insert_run_locked(heap, address + need, FreeRun{run.size - extra, old.segment_base});
      }
      segment.used_bytes += extra;
      old_it->second.block_size = need;
      old_it->second.requested_size = requested;
      zero_growth(address);
      return address;
    }
  }

  const auto new_address = allocate_locked(normalized, flags, requested);
  if (!new_address) return 0;

  const auto copy_size = (std::min)(old.requested_size, requested);
  if (copy_size) {
    std::vector<std::byte> bytes(copy_size);
    if (!memory_.read_bytes(address, bytes) || !memory_.write_bytes(new_address, bytes)) {
      static_cast<void>(free_locked(new_address));
      return 0;
    }
  }
  zero_growth(new_address);

  if (!free_locked(address)) {
    // Do not lose ownership metadata if the old block could not be released.
    static_cast<void>(free_locked(new_address));
    return 0;
  }
  return new_address;
}

void GuestHeapManager::release_all() noexcept {
  std::scoped_lock lock(mutex_);
  for (const auto& [address, allocation] : allocations_) {
    if (allocation.segment_base == 0u) {
      static_cast<void>(memory_.free_virtual(address));
    }
  }
  for (const auto& [handle, heap] : heaps_) {
    static_cast<void>(handle);
    for (const auto& [base, segment] : heap.segments) {
      static_cast<void>(segment);
      static_cast<void>(memory_.free_virtual(base));
    }
  }
  allocations_.clear();
  heaps_.clear();
}

std::size_t GuestHeapManager::outstanding_allocations() const noexcept {
  std::scoped_lock lock(mutex_);
  return allocations_.size();
}

std::size_t GuestHeapManager::segment_count() const noexcept {
  std::scoped_lock lock(mutex_);
  std::size_t count = 0;
  for (const auto& [handle, heap] : heaps_) {
    static_cast<void>(handle);
    count += heap.segments.size();
  }
  return count;
}

bool GuestHeapManager::owns(std::uint32_t heap_handle,
                            std::uint32_t address) const noexcept {
  const auto normalized = normalize_heap_handle(heap_handle);
  std::scoped_lock lock(mutex_);
  const auto it = allocations_.find(address);
  return it != allocations_.end() && it->second.heap_handle == normalized;
}

}  // namespace xenon::kernel
