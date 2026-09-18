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

// Host virtual-memory primitives only. Xbox allocation/protection policy lives
// above this layer in AddressSpace; these functions deliberately know nothing
// about guest addresses, aliases, XEX mappings or Xenon memory types.
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

}  // namespace xenon::memory::host_vm
