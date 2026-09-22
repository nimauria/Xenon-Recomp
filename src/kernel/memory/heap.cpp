#include "xenon/kernel/heap.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "xenon/kernel/memory.hpp"
#include "xenon/memory/types.hpp"

namespace xenon::kernel {
namespace {

constexpr std::uint32_t kDefaultProcessHeapIdentity = 1u;

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
  const auto allocation_size = (std::max)(size, 1u);
  std::uint32_t address = 0;
  const bool zero_initialize = (flags & kHeapZeroMemory) != 0u;

  // Memory V2 owns reservation, commitment, permissions, coherency and
  // lifetime. Its allocator already supplies stronger-than-RTL alignment
  // (64 KiB today), which therefore satisfies ordinary heap alignment too.
  if (!memory_.allocate_virtual(address, allocation_size, memory::kReadWrite,
                                false, zero_initialize)) {
    return 0;
  }

  memory::MappingInfo mapping{};
  if (!memory_.query_virtual(address, mapping)) {
    static_cast<void>(memory_.free_virtual(address));
    return 0;
  }

  known_heap_handles_.insert(heap_handle);
  allocations_.emplace(address,
                       Allocation{heap_handle, allocation_size,
                                  mapping.allocation_size});
  freed_addresses_.erase(address);
  return address;
}

bool GuestHeapManager::free(std::uint32_t heap_handle, std::uint32_t,
                            std::uint32_t address) noexcept {
  if (!address) return true;
  const auto normalized = normalize_heap_handle(heap_handle);
  std::scoped_lock lock(mutex_);
  const auto it = allocations_.find(address);
  if (it == allocations_.end() || it->second.heap_handle != normalized) {
    // Both an invalid pointer and a second free are rejected instead of being
    // silently accepted. Keep a tombstone so debugger/tests can distinguish
    // that the address was previously owned if needed without retaining the
    // allocation itself.
    return false;
  }
  if (!memory_.free_virtual(address)) return false;
  allocations_.erase(it);
  freed_addresses_.insert(address);
  return true;
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
  const auto physical_new_size = (std::max)(new_size, 1u);

  // Keep the same mapping when the existing committed allocation is already
  // large enough. This is the common shrink/small-grow path and preserves the
  // guest address while updating RTL-visible requested-size metadata.
  if (physical_new_size <= old.committed_size) {
    old_it->second.requested_size = physical_new_size;
    if ((flags & kHeapZeroMemory) != 0u &&
        physical_new_size > old.requested_size) {
      memory_.address_space().fill_bytes(
          address + old.requested_size,
          physical_new_size - old.requested_size, 0);
    }
    return address;
  }

  const auto new_address = allocate_locked(normalized, flags, new_size);
  if (!new_address) return 0;

  const auto copy_size = (std::min)(old.requested_size, physical_new_size);
  if (copy_size) {
    std::vector<std::byte> bytes(copy_size);
    if (!memory_.read_bytes(address, bytes) ||
        !memory_.write_bytes(new_address, bytes)) {
      static_cast<void>(memory_.free_virtual(new_address));
      allocations_.erase(new_address);
      return 0;
    }
  }

  if ((flags & kHeapZeroMemory) != 0u &&
      physical_new_size > old.requested_size) {
    memory_.address_space().fill_bytes(
        new_address + old.requested_size,
        physical_new_size - old.requested_size, 0);
  }

  if (!memory_.free_virtual(address)) {
    // Do not lose ownership metadata if the old mapping could not be released.
    static_cast<void>(memory_.free_virtual(new_address));
    allocations_.erase(new_address);
    return 0;
  }
  allocations_.erase(address);
  freed_addresses_.insert(address);
  return new_address;
}

void GuestHeapManager::release_all() noexcept {
  std::scoped_lock lock(mutex_);
  for (const auto& [address, allocation] : allocations_) {
    static_cast<void>(allocation);
    static_cast<void>(memory_.free_virtual(address));
  }
  allocations_.clear();
  freed_addresses_.clear();
  known_heap_handles_.clear();
}

std::size_t GuestHeapManager::outstanding_allocations() const noexcept {
  std::scoped_lock lock(mutex_);
  return allocations_.size();
}

bool GuestHeapManager::owns(std::uint32_t heap_handle,
                            std::uint32_t address) const noexcept {
  const auto normalized = normalize_heap_handle(heap_handle);
  std::scoped_lock lock(mutex_);
  const auto it = allocations_.find(address);
  return it != allocations_.end() && it->second.heap_handle == normalized;
}

}  // namespace xenon::kernel
