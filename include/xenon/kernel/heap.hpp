#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace xenon::kernel {

class KernelMemory;

// Process-owned Xbox guest heap service used by native RTL heap replacements.
// Allocations always live in the existing Memory V2 guest virtual address
// space; no host pointer is ever exposed as a guest address.
class GuestHeapManager final {
 public:
  static constexpr std::uint32_t kHeapZeroMemory = 0x00000008u;
  static constexpr std::uint32_t kInvalidSize = 0xFFFFFFFFu;

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

 private:
  struct Allocation {
    std::uint32_t heap_handle{};
    std::uint32_t requested_size{};
    std::uint32_t committed_size{};
  };

  [[nodiscard]] static std::uint32_t normalize_heap_handle(
      std::uint32_t heap_handle) noexcept;
  [[nodiscard]] std::uint32_t allocate_locked(std::uint32_t heap_handle,
                                              std::uint32_t flags,
                                              std::uint32_t size);

  KernelMemory& memory_;
  mutable std::mutex mutex_;
  std::unordered_map<std::uint32_t, Allocation> allocations_;
  std::unordered_set<std::uint32_t> known_heap_handles_;
  std::unordered_set<std::uint32_t> freed_addresses_;
};

}  // namespace xenon::kernel
