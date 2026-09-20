#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "xenon/cpu/memory_port.hpp"
#include "xenon/cpu/state.hpp"

namespace xenon::core {

// Common native-call bridge for Xbox system exports
// Handles PPC calling convention, argument extraction, return values,
// guest pointers, structures, endian conversion, and faults

class CallBridge {
 public:
  explicit CallBridge(cpu::CpuState& state, cpu::MemoryPort& memory)
      : state_(state), memory_(memory) {}

  // Read 32-bit integer argument
  // Arguments 0-7 are in r3-r10
  // Arguments 8+ are on the stack at r1+0x54 + (index-8)*8
  [[nodiscard]] std::optional<std::uint32_t> read_u32(std::size_t index) const;

  // Read 64-bit integer argument
  [[nodiscard]] std::optional<std::uint64_t> read_u64(std::size_t index) const;

  // Read guest pointer (32-bit address)
  [[nodiscard]] std::optional<cpu::GuestAddress> read_pointer(std::size_t index) const;

  // Read string from guest memory (null-terminated)
  [[nodiscard]] std::optional<std::string> read_string(cpu::GuestAddress address,
                                                      std::size_t max_length = 4096) const;

  // Read wide string from guest memory (null-terminated, big-endian UTF-16)
  [[nodiscard]] std::optional<std::wstring> read_wstring(cpu::GuestAddress address,
                                                         std::size_t max_length = 2048) const;

  // Read arbitrary data from guest memory
  [[nodiscard]] bool read_bytes(cpu::GuestAddress address,
                               std::span<std::byte> destination) const;

  // Write arbitrary data to guest memory
  [[nodiscard]] bool write_bytes(cpu::GuestAddress address,
                                std::span<const std::byte> source);

  // Read big-endian 16-bit value from guest memory
  [[nodiscard]] std::optional<std::uint16_t> read_u16_be(cpu::GuestAddress address) const;

  // Read big-endian 32-bit value from guest memory
  [[nodiscard]] std::optional<std::uint32_t> read_u32_be(cpu::GuestAddress address) const;

  // Read big-endian 64-bit value from guest memory
  [[nodiscard]] std::optional<std::uint64_t> read_u64_be(cpu::GuestAddress address) const;

  // Write big-endian 16-bit value to guest memory
  [[nodiscard]] bool write_u16_be(cpu::GuestAddress address, std::uint16_t value);

  // Write big-endian 32-bit value to guest memory
  [[nodiscard]] bool write_u32_be(cpu::GuestAddress address, std::uint32_t value);

  // Write big-endian 64-bit value to guest memory
  [[nodiscard]] bool write_u64_be(cpu::GuestAddress address, std::uint64_t value);

  // Set 32-bit return value (r3)
  void set_u32_result(std::uint32_t value) { state_.gpr[3] = value; }

  // Set 64-bit return value (r3:r4)
  void set_u64_result(std::uint64_t value) {
    state_.gpr[3] = static_cast<std::uint32_t>(value >> 32);
    state_.gpr[4] = static_cast<std::uint32_t>(value & 0xFFFFFFFFu);
  }

  // Set pointer return value (r3)
  void set_pointer_result(cpu::GuestAddress address) {
    state_.gpr[3] = address;
  }

  // Access to CPU state and memory
  [[nodiscard]] cpu::CpuState& state() { return state_; }
  [[nodiscard]] const cpu::CpuState& state() const { return state_; }
  [[nodiscard]] cpu::MemoryPort& memory() { return memory_; }
  [[nodiscard]] const cpu::MemoryPort& memory() const { return memory_; }

 private:
  cpu::CpuState& state_;
  cpu::MemoryPort& memory_;
};

}  // namespace xenon::core
