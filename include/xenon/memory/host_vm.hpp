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
// page-aligned addresses and page-sized ranges. Shared mapping offsets must be
// aligned to allocation_granularity(), which is 64 KiB on Windows and normally
// the system page size on POSIX hosts.
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
[[nodiscard]] bool map_shared_fixed(
    const SharedMemory& shared, void* address, std::size_t offset,
    std::size_t size,
    Protection protection = Protection::ReadWrite) noexcept;
// Replaces a fixed shared view with an inaccessible anonymous reservation,
// keeping the enclosing aperture address range owned by Xenon.
[[nodiscard]] bool restore_reservation(void* address, std::size_t size) noexcept;

}  // namespace xenon::memory::host_vm
