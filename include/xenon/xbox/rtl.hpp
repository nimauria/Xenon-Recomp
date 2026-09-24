#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "xenon/cpu/memory_port.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::xbox {

// XEX optional-header semantics shared between the loader and RTL exports.
// The low byte of an optional-header key selects how the entry's value field
// is interpreted:
//   0x00: inline value (use raw directly)
//   0x01: inline offset (return address of value field within header)
//   0xFF, 0x02-0xFE: offset field (raw is offset from header base)
struct XexOptionalHeaderView {
  std::uint32_t key{};
  std::uint32_t raw{};
  std::uint32_t entry_offset{};  // Offset within header where this entry resides
};

// Find an optional header entry matching the given key.
// Returns true if found, false otherwise.
// Safe to call with untrusted guest header pointers (validates bounds).
bool find_xex_optional_header(
    cpu::MemoryPort& memory,
    cpu::GuestAddress header_address,
    std::uint32_t key,
    XexOptionalHeaderView& out,
    std::string* error = nullptr);

// Resolve an optional header field and return the result according to Xbox
// semantics:
//   - If key's low byte is 0x00: returns the inline value
//   - If key's low byte is 0x01: returns a guest pointer to the value field
//   - Otherwise: returns header_base + value_or_offset
// Returns std::nullopt if the field is not found or on error.
std::optional<std::uint32_t> resolve_xex_optional_header_field(
    cpu::MemoryPort& memory,
    cpu::GuestAddress header_address,
    std::uint32_t key,
    std::string* error = nullptr);

}  // namespace xenon::xbox
