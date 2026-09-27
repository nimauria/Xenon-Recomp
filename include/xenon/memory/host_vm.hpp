#pragma once

#include <cstddef>
#include <cstdint>

namespace xenon::memory::host_vm {

enum class Protection : std::uint8_t {
  None,
  Read,
  ReadWrite,
  ReadExecute,
  ReadWriteExecute,
};

// Host VM facts that affect which Memory V2 acceleration paths are safe.
// Guest/Xbox page semantics remain fixed at 4 KiB above this layer; these
// values describe only what the host OS can map efficiently and correctly.
struct Capabilities {
  std::size_t page_size{};
  std::size_t allocation_granularity{};
  std::size_t fixed_shared_mapping_granularity{};
  bool fixed_shared_mapping{};
  bool fixed_shared_mapping_requires_page_views{};

  [[nodiscard]] bool supports_fixed_mapping_granularity(
      std::size_t requested_granularity) const noexcept {
    return fixed_shared_mapping && fixed_shared_mapping_granularity != 0u &&
           requested_granularity != 0u &&
           requested_granularity >= fixed_shared_mapping_granularity &&
           (requested_granularity % fixed_shared_mapping_granularity) == 0u;
  }
};

// Opaque host-backed shared-memory object. The native handle is deliberately
// hidden so Windows/POSIX details never escape the host-VM layer. SharedMemory
// is move-only and releases its native object automatically.
class SharedMemory {
 public:
  SharedMemory() noexcept = default;
  ~SharedMemory();

  SharedMemory(const SharedMemory&) = delete;
  SharedMemory& operator=(const SharedMemory&) = delete;

  SharedMemory(SharedMemory&& other) noexcept;
  SharedMemory& operator=(SharedMemory&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept { return native_ != 0; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
  friend SharedMemory create_shared(std::size_t size) noexcept;
  friend void* map_shared(const SharedMemory& shared, std::size_t offset,
                          std::size_t size, Protection protection) noexcept;
  friend bool map_shared_fixed(const SharedMemory& shared, void* address,
                               std::size_t offset, std::size_t size,
                               Protection protection) noexcept;

  std::uintptr_t native_{};
  std::size_t size_{};
};

// Host virtual-memory primitives only. Xbox allocation/protection policy lives
// above this layer in AddressSpace; these functions deliberately know nothing
// about guest addresses, aliases, XEX mappings or Xenon memory types.
//
// Address/range operations use host-page granularity. Callers should pass
// page-aligned addresses and page-sized ranges. Ordinary shared mapping
// offsets must be aligned to allocation_granularity(), which is 64 KiB on
// Windows and normally the system page size on POSIX hosts. Fixed placeholder
// replacement may advertise finer page-sized offsets through
// supports_fixed_shared_mapping().
[[nodiscard]] std::size_t page_size() noexcept;
[[nodiscard]] std::size_t allocation_granularity() noexcept;
[[nodiscard]] void* reserve(std::size_t size) noexcept;
[[nodiscard]] bool commit(void* address, std::size_t size,
                          Protection protection = Protection::ReadWrite) noexcept;
[[nodiscard]] bool decommit(void* address, std::size_t size) noexcept;
[[nodiscard]] bool protect(void* address, std::size_t size,
                           Protection protection) noexcept;
[[nodiscard]] bool discard(void* address, std::size_t size,
                           Protection committed_protection =
                               Protection::ReadWrite) noexcept;
void release(void* address, std::size_t size) noexcept;

// Creates a host shared-memory object suitable for mapping the same bytes at
// multiple host virtual addresses. This is a host mechanism only; Xbox aliasing
// and lifetime semantics remain owned by AddressSpace.
[[nodiscard]] SharedMemory create_shared(std::size_t size) noexcept;
[[nodiscard]] void* map_shared(
    const SharedMemory& shared, std::size_t offset, std::size_t size,
    Protection protection = Protection::ReadWrite) noexcept;
[[nodiscard]] bool unmap(void* address, std::size_t size) noexcept;

// Optional fixed-address shared mappings used by Memory V2's direct guest
// aperture. A backend returning false from supports_fixed_shared_mapping()
// must leave the portable compact page-table path fully functional.
[[nodiscard]] bool supports_fixed_shared_mapping() noexcept;
// Smallest offset/size granularity accepted by map_shared_fixed() on this
// backend. Returns zero when fixed shared mappings are unavailable. This is
// intentionally distinct from allocation_granularity(): modern Windows can
// replace placeholders at 4 KiB even though ordinary section views use a
// 64 KiB allocation granularity.
[[nodiscard]] std::size_t fixed_shared_mapping_granularity() noexcept;
// Reserve/release the enclosing address range used by a direct guest aperture.
// Windows uses placeholder reservations so 4 KiB section offsets can replace
// slices despite the ordinary 64 KiB MapViewOfFile allocation granularity.
[[nodiscard]] void* reserve_fixed_shared_mapping_region(
    std::size_t size) noexcept;
void release_fixed_shared_mapping_region(void* address,
                                         std::size_t size) noexcept;
// Windows placeholder replacement is kept page-view-sized so later guest
// remaps can replace individual 4 KiB translations without tearing down a
// neighboring view. POSIX MAP_FIXED can replace arbitrary subranges directly.
[[nodiscard]] bool fixed_shared_mapping_requires_page_views() noexcept;
[[nodiscard]] bool map_shared_fixed(
    const SharedMemory& shared, void* address, std::size_t offset,
    std::size_t size,
    Protection protection = Protection::ReadWrite) noexcept;
// Replaces fixed shared views with inaccessible reserved address-space slices,
// keeping the enclosing aperture address range owned by Xenon.
[[nodiscard]] bool restore_reservation(void* address, std::size_t size) noexcept;

[[nodiscard]] inline Capabilities capabilities() noexcept {
  return Capabilities{
      page_size(),
      allocation_granularity(),
      fixed_shared_mapping_granularity(),
      supports_fixed_shared_mapping(),
      fixed_shared_mapping_requires_page_views(),
  };
}

}  // namespace xenon::memory::host_vm
