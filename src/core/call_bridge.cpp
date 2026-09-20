#include "xenon/core/call_bridge.hpp"

#include <algorithm>
#include <cstring>

namespace xenon::core {

std::optional<std::uint32_t> CallBridge::read_u32(std::size_t index) const {
  // Arguments 0-7 are in r3-r10
  if (index < 8) {
    return static_cast<std::uint32_t>(state_.gpr[3 + index]);
  }

  // Arguments 8+ are on the stack at r1+0x54 + (index-8)*8
  const auto stack_offset = 0x54u + static_cast<std::uint32_t>((index - 8) * 8);
  const auto stack_address = static_cast<std::uint32_t>(state_.gpr[1]) + stack_offset;

  return read_u32_be(stack_address);
}

std::optional<std::uint64_t> CallBridge::read_u64(std::size_t index) const {
  // For 64-bit values, read high and low 32-bit parts
  auto high = read_u32(index);
  auto low = read_u32(index + 1);

  if (!high || !low) {
    return std::nullopt;
  }

  return (static_cast<std::uint64_t>(*high) << 32) | *low;
}

std::optional<cpu::GuestAddress> CallBridge::read_pointer(std::size_t index) const {
  return read_u32(index);
}

std::optional<std::string> CallBridge::read_string(cpu::GuestAddress address,
                                                  std::size_t max_length) const {
  if (address == 0) {
    return std::nullopt;
  }

  std::string result;
  result.reserve(std::min(max_length, std::size_t{256}));

  try {
    for (std::size_t i = 0; i < max_length; ++i) {
      const auto byte = memory_.read8(address + static_cast<std::uint32_t>(i));
      if (byte == 0) break;
      result.push_back(static_cast<char>(byte));
    }
  } catch (...) {
    // Guest memory fault (unmapped/protected address) while reading.
    return std::nullopt;
  }

  return result;
}

std::optional<std::wstring> CallBridge::read_wstring(cpu::GuestAddress address,
                                                    std::size_t max_length) const {
  if (address == 0) {
    return std::nullopt;
  }

  std::wstring result;
  result.reserve(std::min(max_length, std::size_t{256}));

  try {
    for (std::size_t i = 0; i < max_length; ++i) {
      const auto code_unit = memory_.read16_be(address + static_cast<std::uint32_t>(i * 2));
      if (code_unit == 0) break;
      result.push_back(static_cast<wchar_t>(code_unit));
    }
  } catch (...) {
    return std::nullopt;
  }

  return result;
}

bool CallBridge::read_bytes(cpu::GuestAddress address,
                           std::span<std::byte> destination) const {
  try {
    for (std::size_t i = 0; i < destination.size(); ++i) {
      const auto byte = memory_.read8(address + static_cast<std::uint32_t>(i));
      destination[i] = static_cast<std::byte>(byte);
    }
  } catch (...) {
    return false;
  }
  return true;
}

bool CallBridge::write_bytes(cpu::GuestAddress address,
                            std::span<const std::byte> source) {
  try {
    for (std::size_t i = 0; i < source.size(); ++i) {
      memory_.write8(address + static_cast<std::uint32_t>(i),
                     static_cast<std::uint8_t>(source[i]));
    }
  } catch (...) {
    return false;
  }
  return true;
}

std::optional<std::uint16_t> CallBridge::read_u16_be(
    cpu::GuestAddress address) const {
  try {
    return memory_.read16_be(address);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::uint32_t> CallBridge::read_u32_be(
    cpu::GuestAddress address) const {
  try {
    return memory_.read32_be(address);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::uint64_t> CallBridge::read_u64_be(
    cpu::GuestAddress address) const {
  try {
    return memory_.read64_be(address);
  } catch (...) {
    return std::nullopt;
  }
}

bool CallBridge::write_u16_be(cpu::GuestAddress address, std::uint16_t value) {
  try {
    memory_.write16_be(address, value);
  } catch (...) {
    return false;
  }
  return true;
}

bool CallBridge::write_u32_be(cpu::GuestAddress address, std::uint32_t value) {
  try {
    memory_.write32_be(address, value);
  } catch (...) {
    return false;
  }
  return true;
}

bool CallBridge::write_u64_be(cpu::GuestAddress address, std::uint64_t value) {
  try {
    memory_.write64_be(address, value);
  } catch (...) {
    return false;
  }
  return true;
}

}  // namespace xenon::core
