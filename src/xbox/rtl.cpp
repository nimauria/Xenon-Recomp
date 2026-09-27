#include "xenon/xbox/rtl.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace xenon::xbox {
namespace {

// Read a big-endian 32-bit value from guest memory
std::optional<std::uint32_t> read_guest_be32(cpu::MemoryPort& memory,
                                             cpu::GuestAddress address) {
  try {
    return memory.read32_be(address);
  } catch (const memory::MemoryFault&) {
    return std::nullopt;
  }
}

// Read a big-endian 16-bit value from guest memory
std::optional<std::uint16_t> read_guest_be16(cpu::MemoryPort& memory,
                                             cpu::GuestAddress address) {
  try {
    return memory.read16_be(address);
  } catch (const memory::MemoryFault&) {
    return std::nullopt;
  }
}

// Constants for XEX optional header parsing
constexpr std::size_t kXexRootHeaderMinSize = 0x18u;  // Base header before optional table
constexpr std::size_t kOptionalHeaderEntrySize = 8u;   // key(4) + value(4)
constexpr std::size_t kMaxOptionalHeaders = 128u;      // Prevent DoS on malformed count
constexpr std::size_t kMaxHeaderSize = 16u * 1024u * 1024u;  // Bound allocation

std::optional<cpu::GuestAddress> checked_guest_address_add(
    cpu::GuestAddress base, std::uint64_t offset) {
  const auto result = static_cast<std::uint64_t>(base) + offset;
  if (result > std::numeric_limits<cpu::GuestAddress>::max()) {
    return std::nullopt;
  }
  return static_cast<cpu::GuestAddress>(result);
}

}  // namespace

bool find_xex_optional_header(cpu::MemoryPort& memory,
                              cpu::GuestAddress header_address,
                              std::uint32_t key,
                              XexOptionalHeaderView& out,
                              std::string* error) {
  if (header_address == 0u) {
    if (error) *error = "Null XEX header address";
    return false;
  }

  // Read the root header to get the header size and optional header count
  // XEX2 root header layout:
  //   +0x00: magic (4 bytes)
  //   +0x04: module_flags (4 bytes)
  //   +0x08: header_size (4 bytes)
  //   +0x0C: image_size (4 bytes)
  //   +0x10: security_info_offset (4 bytes)
  //   +0x14: optional_header_count (4 bytes)
  //   +0x18: optional_header_table[]
  const auto optional_count_address =
      checked_guest_address_add(header_address, 0x14u);
  if (!optional_count_address) {
    if (error) *error = "XEX header address overflow";
    return false;
  }
  const auto opt_count_result =
      read_guest_be32(memory, *optional_count_address);
  if (!opt_count_result) {
    if (error) *error = "Failed to read optional header count";
    return false;
  }

  const auto optional_header_count = *opt_count_result;
  if (optional_header_count > kMaxOptionalHeaders) {
    if (error) *error = "Optional header count exceeds maximum";
    return false;
  }

  // Bound the search to avoid reading past the header bounds
  const auto header_size_address = checked_guest_address_add(header_address, 0x08u);
  if (!header_size_address) {
    if (error) *error = "XEX header address overflow";
    return false;
  }
  const auto header_size_result = read_guest_be32(memory, *header_size_address);
  if (!header_size_result) {
    if (error) *error = "Failed to read header size";
    return false;
  }

  const auto header_size = *header_size_result;
  if (header_size < kXexRootHeaderMinSize || header_size > kMaxHeaderSize) {
    if (error) *error = "Invalid XEX header size";
    return false;
  }

  // Iterate over optional header entries
  for (std::uint32_t i = 0u; i < optional_header_count; ++i) {
    const auto entry_offset =
        static_cast<std::uint64_t>(0x18u) +
        static_cast<std::uint64_t>(i) * kOptionalHeaderEntrySize;
    if (entry_offset > header_size ||
        kOptionalHeaderEntrySize > header_size - entry_offset ||
        entry_offset > std::numeric_limits<std::uint32_t>::max()) {
      if (error) *error = "Optional header table extends past header";
      return false;
    }

    const auto entry_address =
        checked_guest_address_add(header_address, entry_offset);
    if (!entry_address) {
      if (error) *error = "Optional header address overflow";
      return false;
    }
    const auto entry_key_result = read_guest_be32(memory, *entry_address);
    if (!entry_key_result) {
      if (error) *error = "Failed to read optional header entry key";
      return false;
    }

    const auto entry_key = *entry_key_result;
    if (entry_key == key) {
      // Found the matching key
      const auto value_address =
          checked_guest_address_add(*entry_address, 4u);
      if (!value_address) {
        if (error) *error = "Optional header value address overflow";
        return false;
      }
      const auto entry_value_result = read_guest_be32(memory, *value_address);
      if (!entry_value_result) {
        if (error) *error = "Failed to read optional header entry value";
        return false;
      }

      out.key = entry_key;
      out.raw = *entry_value_result;
      out.entry_offset = static_cast<std::uint32_t>(entry_offset);
      return true;
    }
  }

  // Key not found
  return false;
}

std::optional<std::uint32_t> resolve_xex_optional_header_field(
    cpu::MemoryPort& memory,
    cpu::GuestAddress header_address,
    std::uint32_t key,
    std::string* error) {
  if (header_address == 0u) {
    return std::nullopt;
  }

  XexOptionalHeaderView entry{};
  if (!find_xex_optional_header(memory, header_address, key, entry, error)) {
    return std::nullopt;
  }

  const auto size_class = key & 0xFFu;
  if (size_class == 0x00u) {
    // Inline value: return the raw value directly
    return entry.raw;
  } else if (size_class == 0x01u) {
    // Inline offset: return a pointer to the value field within the header
    // The value is at: header_address + entry_offset + 4 (after the key field)
    return (header_address + entry.entry_offset + 4u) & 0xFFFFFFFFu;
  } else {
    // Offset-backed: entry.raw is an offset from the header base
    // Bounds check: ensure the offset doesn't escape the header
    const auto header_size_address =
        checked_guest_address_add(header_address, 0x08u);
    if (!header_size_address) {
      if (error) *error = "XEX header address overflow";
      return std::nullopt;
    }
    const auto header_size_result = read_guest_be32(memory, *header_size_address);
    if (!header_size_result) {
      if (error) *error = "Failed to read header size for offset resolution";
      return std::nullopt;
    }

    const auto header_size = *header_size_result;
    if (entry.raw >= header_size) {
      if (error) *error = "Optional header offset exceeds header size";
      return std::nullopt;
    }

    const auto result = checked_guest_address_add(header_address, entry.raw);
    if (!result) {
      if (error) *error = "Optional header result address overflow";
      return std::nullopt;
    }
    return *result;
  }
}

}  // namespace xenon::xbox
