#include "xenon/kernel/memory.hpp"

namespace xenon::kernel {

KernelMemory::KernelMemory(std::shared_ptr<memory::AddressSpace> address_space)
    : address_space_(std::move(address_space)) {}

bool KernelMemory::allocate_virtual(
    std::uint32_t& address, std::uint32_t size,
    memory::Protect protect, bool top_down, bool zero_initialize) {
  memory::GuestAddress guest_address = address;
  constexpr std::uint32_t kDefaultAlign = 64 * 1024;  // 64KB alignment
  memory::VirtualAllocationOptions options{};
  options.commit = true;
  options.zero_initialize = zero_initialize;
  if (!address_space_->allocate(size, kDefaultAlign, protect, top_down, guest_address,
                                std::nullopt, options)) {
    return false;
  }
  address = static_cast<std::uint32_t>(guest_address);
  return true;
}

bool KernelMemory::free_virtual(std::uint32_t address) {
  return address_space_->release(address);
}

bool KernelMemory::protect_virtual(
    std::uint32_t address, std::uint32_t size,
    memory::Protect new_protect, memory::Protect* old_protect) {
  return address_space_->protect(address, size, new_protect, old_protect);
}

bool KernelMemory::query_virtual(
    std::uint32_t address, memory::MappingInfo& info) const {
  auto result = address_space_->query(address);
  if (!result) {
    return false;
  }
  info = *result;
  return true;
}

bool KernelMemory::allocate_physical(
    std::uint32_t size, std::uint32_t alignment,
    std::uint32_t& physical_address, bool top_down) {
  return address_space_->allocate_physical(size, alignment, top_down, physical_address);
}

bool KernelMemory::free_physical(
    std::uint32_t physical_address, std::uint32_t size) {
  return address_space_->free_physical(physical_address, size);
}

bool KernelMemory::map_physical(
    std::uint32_t virtual_address, std::uint32_t physical_address,
    std::uint32_t size, memory::Protect protect) {
  return address_space_->map_virtual_to_physical(
      virtual_address, physical_address, size, protect);
}

bool KernelMemory::read_bytes(
    std::uint32_t address, std::span<std::byte> destination) const {
  try {
    address_space_->read_bytes(address, destination);
    return true;
  } catch (...) {
    return false;
  }
}

bool KernelMemory::write_bytes(
    std::uint32_t address, std::span<const std::byte> source) {
  try {
    address_space_->write_bytes(address, source);
    return true;
  } catch (...) {
    return false;
  }
}

std::uint32_t KernelMemory::read32_be(std::uint32_t address) const {
  return address_space_->fetch32_be(address);
}

bool KernelMemory::write32_be(std::uint32_t address, std::uint32_t value) {
  std::array<std::byte, 4> bytes{
      static_cast<std::byte>((value >> 24) & 0xFF),
      static_cast<std::byte>((value >> 16) & 0xFF),
      static_cast<std::byte>((value >> 8) & 0xFF),
      static_cast<std::byte>(value & 0xFF)};
  return write_bytes(address, bytes);
}

}  // namespace xenon::kernel
