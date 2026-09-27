#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "xenon/memory/address_space.hpp"
#include "xenon/memory/types.hpp"

namespace xenon::kernel {

// Kernel memory management - connects xboxkrnl memory exports to Memory V2
class KernelMemory {
 public:
  explicit KernelMemory(std::shared_ptr<memory::AddressSpace> address_space);

  // Virtual memory allocation
  [[nodiscard]] bool allocate_virtual(
      std::uint32_t& address, std::uint32_t size,
      memory::Protect protect, bool top_down = false,
      bool zero_initialize = true);

  [[nodiscard]] bool free_virtual(std::uint32_t address);

  [[nodiscard]] bool protect_virtual(
      std::uint32_t address, std::uint32_t size,
      memory::Protect new_protect, memory::Protect* old_protect = nullptr);

  [[nodiscard]] bool query_virtual(
      std::uint32_t address, memory::MappingInfo& info) const;

  // Physical memory allocation
  [[nodiscard]] bool allocate_physical(
      std::uint32_t size, std::uint32_t alignment,
      std::uint32_t& physical_address, bool top_down = false);

  [[nodiscard]] bool free_physical(
      std::uint32_t physical_address, std::uint32_t size);

  [[nodiscard]] bool map_physical(
      std::uint32_t virtual_address, std::uint32_t physical_address,
      std::uint32_t size, memory::Protect protect);

  // Read/Write helpers
  [[nodiscard]] bool read_bytes(
      std::uint32_t address, std::span<std::byte> destination) const;

  [[nodiscard]] bool write_bytes(
      std::uint32_t address, std::span<const std::byte> source);

  [[nodiscard]] std::uint32_t read32_be(std::uint32_t address) const;
  [[nodiscard]] bool write32_be(std::uint32_t address, std::uint32_t value);

  // Access the underlying address space
  [[nodiscard]] memory::AddressSpace& address_space() const {
    return *address_space_;
  }

 private:
  std::shared_ptr<memory::AddressSpace> address_space_;
};

}  // namespace xenon::kernel
