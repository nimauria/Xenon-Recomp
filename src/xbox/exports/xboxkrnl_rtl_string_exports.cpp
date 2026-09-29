#include "xenon/xbox/xboxkrnl_rtl_string_exports.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

#include "xenon/core/export_registry.hpp"
#include "xenon/cpu/memory_port.hpp"

namespace xenon::xbox {
namespace {

using core::ExportCallContext;

struct CaseMapEntry {
  std::uint16_t from;
  std::uint16_t to;
};
#include "../unicode_case_tables.inc"

std::uint16_t map_case(const CaseMapEntry* begin, const CaseMapEntry* end, std::uint16_t c) noexcept {
  const auto it = std::lower_bound(begin, end, c,
                                   [](const CaseMapEntry& entry, std::uint16_t value) {
                                     return entry.from < value;
                                   });
  return (it != end && it->from == c) ? it->to : c;
}

// NTSTATUS values used by this family.
constexpr std::uint32_t kStatusSuccess = 0u;
constexpr std::uint32_t kStatusBufferOverflow = 0x80000005u;
constexpr std::uint32_t kStatusNoMemory = 0xC0000017u;
constexpr std::uint32_t kStatusBufferTooSmall = 0xC0000023u;
constexpr std::uint32_t kStatusInvalidParameter = 0xC000000Du;

// ANSI_STRING / UNICODE_STRING: Length (bytes) u16, MaximumLength (bytes) u16,
// Buffer u32 (guest pointer), all big-endian.
struct GuestString {
  std::uint16_t length{};
  std::uint16_t maximum{};
  std::uint32_t buffer{};
};

GuestString read_string(cpu::MemoryPort& memory, cpu::GuestAddress address) {
  return {memory.read16_be(address), memory.read16_be(address + 2u), memory.read32_be(address + 4u)};
}

void write_length(cpu::MemoryPort& memory, cpu::GuestAddress address, std::uint16_t length) {
  memory.write16_be(address, length);
}

void set_result(ExportCallContext& context, std::uint32_t status) {
  context.cpu.gpr[3] = status;
}

void set_int_result(ExportCallContext& context, std::int32_t value) {
  context.cpu.gpr[3] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

// Latin-1 case conversion for the single-byte ("ANSI") character functions.
std::uint8_t latin1_upper(std::uint8_t c) {
  if ((c >= 'a' && c <= 'z') || (c >= 0xE0u && c <= 0xFEu && c != 0xF7u)) {
    return static_cast<std::uint8_t>(c - 0x20u);
  }
  return c;
}
std::uint8_t latin1_lower(std::uint8_t c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 0xC0u && c <= 0xDEu && c != 0xD7u)) {
    return static_cast<std::uint8_t>(c + 0x20u);
  }
  return c;
}

// RtlCompareMemory (0x11A): r3 = source 1, r4 = source 2, r5 = length -> r3 =
// number of leading bytes that are equal.
bool rtl_compare_memory(ExportCallContext& context) {
  const auto a = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto b = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto length = static_cast<std::uint32_t>(context.cpu.gpr[5]);
  std::uint32_t matched = 0;
  while (matched < length && context.memory.read8(a + matched) == context.memory.read8(b + matched)) {
    ++matched;
  }
  context.cpu.gpr[3] = matched;
  return true;
}

std::int32_t compare_bytes(cpu::MemoryPort& memory, cpu::GuestAddress a, std::uint32_t a_length,
                           cpu::GuestAddress b, std::uint32_t b_length, bool ignore_case) {
  const auto common = std::min(a_length, b_length);
  for (std::uint32_t i = 0; i < common; ++i) {
    std::uint8_t c1 = memory.read8(a + i);
    std::uint8_t c2 = memory.read8(b + i);
    if (ignore_case) {
      c1 = latin1_upper(c1);
      c2 = latin1_upper(c2);
    }
    if (c1 != c2) return static_cast<std::int32_t>(c1) - static_cast<std::int32_t>(c2);
  }
  return static_cast<std::int32_t>(a_length) - static_cast<std::int32_t>(b_length);
}

// RtlCompareString (0x11C): r3 = ANSI_STRING* 1, r4 = ANSI_STRING* 2, r5 =
// CaseInSensitive -> r3 = <0, 0, >0. The first differing byte decides
// (upper-cased when case-insensitive); equal common prefixes compare by length.
bool rtl_compare_string(ExportCallContext& context) {
  const auto s1 = read_string(context.memory, static_cast<cpu::GuestAddress>(context.cpu.gpr[3]));
  const auto s2 = read_string(context.memory, static_cast<cpu::GuestAddress>(context.cpu.gpr[4]));
  set_int_result(context, compare_bytes(context.memory, s1.buffer, s1.length, s2.buffer, s2.length,
                                        (context.cpu.gpr[5] & 0xFFu) != 0u));
  return true;
}

// RtlCompareStringN (0x11D): r3 = char* 1, r4 = length 1, r5 = char* 2, r6 =
// length 2, r7 = CaseInSensitive -> r3 as RtlCompareString, over raw buffers.
bool rtl_compare_string_n(ExportCallContext& context) {
  set_int_result(context,
                 compare_bytes(context.memory, static_cast<cpu::GuestAddress>(context.cpu.gpr[3]),
                               static_cast<std::uint32_t>(context.cpu.gpr[4]),
                               static_cast<cpu::GuestAddress>(context.cpu.gpr[5]),
                               static_cast<std::uint32_t>(context.cpu.gpr[6]),
                               (context.cpu.gpr[7] & 0xFFu) != 0u));
  return true;
}

// RtlCompareUnicodeString (0x11E): r3 = UNICODE_STRING* 1, r4 = UNICODE_STRING* 2,
// r5 = CaseInSensitive -> r3 = <0, 0, >0. Compares UTF-16 units (upcased when
// case-insensitive); a common prefix compares by length in bytes.
bool rtl_compare_unicode_string(ExportCallContext& context) {
  const auto s1 = read_string(context.memory, static_cast<cpu::GuestAddress>(context.cpu.gpr[3]));
  const auto s2 = read_string(context.memory, static_cast<cpu::GuestAddress>(context.cpu.gpr[4]));
  const bool ignore_case = (context.cpu.gpr[5] & 0xFFu) != 0u;
  const auto common = std::min(s1.length, s2.length) / 2u;
  for (std::uint32_t i = 0; i < common; ++i) {
    std::uint16_t c1 = context.memory.read16_be(s1.buffer + i * 2u);
    std::uint16_t c2 = context.memory.read16_be(s2.buffer + i * 2u);
    if (ignore_case) {
      c1 = rtl_upcase_unicode_char(c1);
      c2 = rtl_upcase_unicode_char(c2);
    }
    if (c1 != c2) {
      set_int_result(context, static_cast<std::int32_t>(c1) - static_cast<std::int32_t>(c2));
      return true;
    }
  }
  set_int_result(context, static_cast<std::int32_t>(s1.length) - static_cast<std::int32_t>(s2.length));
  return true;
}

void copy_bytes(cpu::MemoryPort& memory, cpu::GuestAddress to, cpu::GuestAddress from,
                std::uint32_t count) {
  for (std::uint32_t i = 0; i < count; ++i) memory.write8(to + i, memory.read8(from + i));
}

// RtlCopyString (0x121): r3 = destination STRING*, r4 = source STRING* (nullable)
// -> void. Copies min(source.Length, destination.MaximumLength) bytes and sets
// destination.Length to the amount copied; a NULL source empties the destination.
bool rtl_copy_string(ExportCallContext& context) {
  const auto dest_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto src_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  if (dest_ptr == 0u) return true;
  const auto dest = read_string(context.memory, dest_ptr);
  if (src_ptr == 0u) {
    write_length(context.memory, dest_ptr, 0u);
    return true;
  }
  const auto src = read_string(context.memory, src_ptr);
  const auto count = std::min<std::uint32_t>(src.length, dest.maximum);
  copy_bytes(context.memory, dest.buffer, src.buffer, count);
  write_length(context.memory, dest_ptr, static_cast<std::uint16_t>(count));
  return true;
}

// RtlCopyUnicodeString (0x122): as RtlCopyString for UNICODE_STRING; the copied
// length is a whole number of UTF-16 units and, if there is room, the result is
// NUL-terminated.
bool rtl_copy_unicode_string(ExportCallContext& context) {
  const auto dest_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto src_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  if (dest_ptr == 0u) return true;
  const auto dest = read_string(context.memory, dest_ptr);
  std::uint32_t count = 0;
  if (src_ptr != 0u) {
    const auto src = read_string(context.memory, src_ptr);
    count = std::min<std::uint32_t>(src.length, dest.maximum) & ~1u;
    copy_bytes(context.memory, dest.buffer, src.buffer, count);
  }
  write_length(context.memory, dest_ptr, static_cast<std::uint16_t>(count));
  if (count + 2u <= dest.maximum) context.memory.write16_be(dest.buffer + count, 0u);
  return true;
}

// RtlAppendStringToString (0x115) / RtlAppendUnicodeStringToString (0x116):
// r3 = destination, r4 = source (nullable) -> r3 = NTSTATUS. Appends the source
// after the destination's current Length; STATUS_BUFFER_TOO_SMALL (with nothing
// written) if it does not fit in MaximumLength. The Unicode form keeps the
// result NUL-terminated when there is room.
bool append_string(ExportCallContext& context, bool unicode) {
  const auto dest_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto src_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  if (src_ptr == 0u) {
    set_result(context, kStatusSuccess);
    return true;
  }
  const auto dest = read_string(context.memory, dest_ptr);
  const auto src = read_string(context.memory, src_ptr);
  if (src.length > 0u) {
    if (static_cast<std::uint32_t>(dest.length) + src.length > dest.maximum) {
      set_result(context, kStatusBufferTooSmall);
      return true;
    }
    copy_bytes(context.memory, dest.buffer + dest.length, src.buffer, src.length);
  }
  const auto total = static_cast<std::uint16_t>(dest.length + src.length);
  write_length(context.memory, dest_ptr, total);
  if (unicode && total + 2u <= dest.maximum) context.memory.write16_be(dest.buffer + total, 0u);
  set_result(context, kStatusSuccess);
  return true;
}

// RtlAppendUnicodeToString (0x117): r3 = destination UNICODE_STRING*, r4 = PCWSTR
// (nullable, NUL-terminated) -> r3 = NTSTATUS.
bool rtl_append_unicode_to_string(ExportCallContext& context) {
  const auto dest_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto src = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  if (src == 0u) {
    set_result(context, kStatusSuccess);
    return true;
  }
  std::uint32_t units = 0;
  while (units < 0x7FFFu && context.memory.read16_be(src + units * 2u) != 0u) ++units;
  const auto dest = read_string(context.memory, dest_ptr);
  const std::uint32_t bytes = units * 2u;
  if (dest.length + bytes > dest.maximum) {
    set_result(context, kStatusBufferTooSmall);
    return true;
  }
  copy_bytes(context.memory, dest.buffer + dest.length, src, bytes);
  const auto total = static_cast<std::uint16_t>(dest.length + bytes);
  write_length(context.memory, dest_ptr, total);
  if (total + 2u <= dest.maximum) context.memory.write16_be(dest.buffer + total, 0u);
  set_result(context, kStatusSuccess);
  return true;
}

// RtlUpperChar (0x14A) / RtlLowerChar (0x132): r3 = a character -> r3 = its
// upper/lower-case form (Latin-1 rules for the single-byte code page).
bool rtl_upper_char(ExportCallContext& context) {
  context.cpu.gpr[3] = latin1_upper(static_cast<std::uint8_t>(context.cpu.gpr[3]));
  return true;
}
bool rtl_lower_char(ExportCallContext& context) {
  context.cpu.gpr[3] = latin1_lower(static_cast<std::uint8_t>(context.cpu.gpr[3]));
  return true;
}

// RtlUpcaseUnicodeChar (0x149) / RtlDowncaseUnicodeChar (0x124): r3 = a UTF-16
// unit -> r3 = its simple case mapping.
bool rtl_upcase_unicode_char_export(ExportCallContext& context) {
  context.cpu.gpr[3] = rtl_upcase_unicode_char(static_cast<std::uint16_t>(context.cpu.gpr[3]));
  return true;
}
bool rtl_downcase_unicode_char_export(ExportCallContext& context) {
  context.cpu.gpr[3] = rtl_downcase_unicode_char(static_cast<std::uint16_t>(context.cpu.gpr[3]));
  return true;
}

// RtlMultiByteToUnicodeN (0x133): r3 = wide destination, r4 = destination
// capacity (bytes), r5 = PULONG bytes written (nullable), r6 = multibyte
// source, r7 = source length (bytes) -> r3 = NTSTATUS. Converts as many bytes
// as fit; each byte becomes the UTF-16 unit of the same value (Latin-1).
bool rtl_multi_byte_to_unicode_n(ExportCallContext& context) {
  const auto dest = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto capacity = static_cast<std::uint32_t>(context.cpu.gpr[4]);
  const auto written_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[5]);
  const auto source = static_cast<cpu::GuestAddress>(context.cpu.gpr[6]);
  const auto source_bytes = static_cast<std::uint32_t>(context.cpu.gpr[7]);
  const auto count = std::min(source_bytes, capacity / 2u);
  for (std::uint32_t i = 0; i < count; ++i) {
    context.memory.write16_be(dest + i * 2u, context.memory.read8(source + i));
  }
  if (written_ptr != 0u) context.memory.write32_be(written_ptr, count * 2u);
  set_result(context, kStatusSuccess);
  return true;
}

// RtlMultiByteToUnicodeSize (0x134): r3 = PULONG receiving the wide byte count,
// r4 = multibyte source, r5 = source length -> r3 = NTSTATUS.
bool rtl_multi_byte_to_unicode_size(ExportCallContext& context) {
  const auto out = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  if (out != 0u) {
    context.memory.write32_be(out, static_cast<std::uint32_t>(context.cpu.gpr[5]) * 2u);
  }
  set_result(context, kStatusSuccess);
  return true;
}

// RtlUnicodeToMultiByteN (0x143): r3 = multibyte destination, r4 = capacity
// (bytes), r5 = PULONG bytes written (nullable), r6 = wide source, r7 = wide
// source length (bytes) -> r3 = NTSTATUS. A unit above 0xFF becomes '?'.
bool rtl_unicode_to_multi_byte_n(ExportCallContext& context) {
  const auto dest = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto capacity = static_cast<std::uint32_t>(context.cpu.gpr[4]);
  const auto written_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[5]);
  const auto source = static_cast<cpu::GuestAddress>(context.cpu.gpr[6]);
  const auto source_bytes = static_cast<std::uint32_t>(context.cpu.gpr[7]);
  const auto count = std::min(source_bytes / 2u, capacity);
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto unit = context.memory.read16_be(source + i * 2u);
    context.memory.write8(dest + i, unit <= 0xFFu ? static_cast<std::uint8_t>(unit) : '?');
  }
  if (written_ptr != 0u) context.memory.write32_be(written_ptr, count);
  set_result(context, kStatusSuccess);
  return true;
}

// RtlUnicodeToMultiByteSize (0x144): r3 = PULONG receiving the multibyte byte
// count, r4 = wide source, r5 = wide length (bytes) -> r3 = NTSTATUS.
bool rtl_unicode_to_multi_byte_size(ExportCallContext& context) {
  const auto out = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  if (out != 0u) {
    context.memory.write32_be(out, static_cast<std::uint32_t>(context.cpu.gpr[5]) / 2u);
  }
  set_result(context, kStatusSuccess);
  return true;
}

}  // namespace

std::uint16_t rtl_upcase_unicode_char(std::uint16_t c) noexcept {
  return map_case(std::begin(kUpcaseTable), std::end(kUpcaseTable), c);
}

std::uint16_t rtl_downcase_unicode_char(std::uint16_t c) noexcept {
  return map_case(std::begin(kDowncaseTable), std::end(kDowncaseTable), c);
}

namespace {

// Conversions between ANSI_STRING and UNICODE_STRING, with the optional
// destination allocation (AllocateDestinationString) and the matching frees.
// Sizes follow the NT contract: the converted string is NUL-terminated, so a
// non-allocating conversion needs MaximumLength > Length and otherwise fills
// what fits and reports STATUS_BUFFER_OVERFLOW.
class StringConversions {
 public:
  explicit StringConversions(RtlPoolHooks hooks) : hooks_(std::move(hooks)) {}

  // RtlUnicodeStringToAnsiString (0x142): r3 = ANSI_STRING* dest, r4 =
  // UNICODE_STRING* source, r5 = AllocateDestinationString -> r3 = NTSTATUS.
  bool unicode_to_ansi(ExportCallContext& context) const {
    const auto dest_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
    const auto src_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
    const bool allocate = (context.cpu.gpr[5] & 0xFFu) != 0u;
    const auto src = read_string(context.memory, src_ptr);
    const std::uint32_t chars = src.length / 2u;
    if (chars + 1u > 0xFFFFu) {
      set_result(context, kStatusInvalidParameter);
      return true;
    }
    auto dest = read_string(context.memory, dest_ptr);
    std::uint32_t convert = chars;
    std::uint32_t status = kStatusSuccess;
    if (allocate) {
      const auto buffer = allocate_buffer(chars + 1u);
      if (buffer == 0u) {
        set_result(context, kStatusNoMemory);
        return true;
      }
      dest.buffer = buffer;
      dest.maximum = static_cast<std::uint16_t>(chars + 1u);
      context.memory.write16_be(dest_ptr + 2u, dest.maximum);
      context.memory.write32_be(dest_ptr + 4u, buffer);
    } else if (dest.maximum <= chars) {
      status = kStatusBufferOverflow;
      convert = dest.maximum > 0u ? dest.maximum - 1u : 0u;
    }
    for (std::uint32_t i = 0; i < convert; ++i) {
      const auto unit = context.memory.read16_be(src.buffer + i * 2u);
      context.memory.write8(dest.buffer + i, unit <= 0xFFu ? static_cast<std::uint8_t>(unit) : '?');
    }
    if (dest.maximum > convert) context.memory.write8(dest.buffer + convert, 0u);
    write_length(context.memory, dest_ptr, static_cast<std::uint16_t>(convert));
    set_result(context, status);
    return true;
  }

  // RtlAnsiStringToUnicodeString (0x114): r3 = UNICODE_STRING* dest, r4 =
  // ANSI_STRING* source, r5 = AllocateDestinationString -> r3 = NTSTATUS.
  bool ansi_to_unicode(ExportCallContext& context) const {
    const auto dest_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
    const auto src_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
    const bool allocate = (context.cpu.gpr[5] & 0xFFu) != 0u;
    const auto src = read_string(context.memory, src_ptr);
    const std::uint32_t bytes = static_cast<std::uint32_t>(src.length) * 2u;
    if (bytes + 2u > 0xFFFFu) {
      set_result(context, kStatusInvalidParameter);
      return true;
    }
    auto dest = read_string(context.memory, dest_ptr);
    std::uint32_t convert = src.length;
    std::uint32_t status = kStatusSuccess;
    if (allocate) {
      const auto buffer = allocate_buffer(bytes + 2u);
      if (buffer == 0u) {
        set_result(context, kStatusNoMemory);
        return true;
      }
      dest.buffer = buffer;
      dest.maximum = static_cast<std::uint16_t>(bytes + 2u);
      context.memory.write16_be(dest_ptr + 2u, dest.maximum);
      context.memory.write32_be(dest_ptr + 4u, buffer);
    } else if (dest.maximum < bytes + 2u) {
      status = kStatusBufferOverflow;
      convert = dest.maximum >= 2u ? (dest.maximum - 2u) / 2u : 0u;
    }
    for (std::uint32_t i = 0; i < convert; ++i) {
      context.memory.write16_be(dest.buffer + i * 2u, context.memory.read8(src.buffer + i));
    }
    if (dest.maximum >= convert * 2u + 2u) context.memory.write16_be(dest.buffer + convert * 2u, 0u);
    write_length(context.memory, dest_ptr, static_cast<std::uint16_t>(convert * 2u));
    set_result(context, status);
    return true;
  }

  // RtlCreateUnicodeString (0x123): r3 = UNICODE_STRING* dest, r4 = PCWSTR source
  // -> r3 = BOOLEAN (1 on success, 0 if the allocation fails). Allocates a
  // NUL-terminated copy.
  bool create_unicode_string(ExportCallContext& context) const {
    const auto dest_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
    const auto src = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
    std::uint32_t units = 0;
    if (src != 0u) {
      while (units < 0x7FFEu && context.memory.read16_be(src + units * 2u) != 0u) ++units;
    }
    const auto buffer = allocate_buffer(units * 2u + 2u);
    if (buffer == 0u) {
      context.cpu.gpr[3] = 0u;
      return true;
    }
    for (std::uint32_t i = 0; i < units; ++i) {
      context.memory.write16_be(buffer + i * 2u, context.memory.read16_be(src + i * 2u));
    }
    context.memory.write16_be(buffer + units * 2u, 0u);
    write_length(context.memory, dest_ptr, static_cast<std::uint16_t>(units * 2u));
    context.memory.write16_be(dest_ptr + 2u, static_cast<std::uint16_t>(units * 2u + 2u));
    context.memory.write32_be(dest_ptr + 4u, buffer);
    context.cpu.gpr[3] = 1u;
    return true;
  }

  // RtlFreeAnsiString (0x127) / RtlFreeUnicodeString (0x128): r3 = STRING* ->
  // void. Releases the buffer if it is a pool allocation (a conversion made with
  // AllocateDestinationString/RtlCreateUnicodeString) - a string that aliases
  // caller-owned memory is left alone - and clears the string header.
  bool free_string(ExportCallContext& context) const {
    const auto string_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
    if (string_ptr == 0u) return true;
    const auto buffer = context.memory.read32_be(string_ptr + 4u);
    if (buffer != 0u && hooks_.free) static_cast<void>(hooks_.free(buffer));
    context.memory.write16_be(string_ptr + 0u, 0u);
    context.memory.write16_be(string_ptr + 2u, 0u);
    context.memory.write32_be(string_ptr + 4u, 0u);
    return true;
  }

 private:
  std::uint32_t allocate_buffer(std::uint32_t bytes) const {
    return hooks_.allocate ? hooks_.allocate(bytes) : 0u;
  }

  RtlPoolHooks hooks_;
};

struct StringExportSpec {
  std::uint32_t ordinal;
  const char* name;
  core::ExportHandler handler;
};

}  // namespace

bool register_xboxkrnl_rtl_string_exports(core::ExportRegistry& registry, RtlPoolHooks hooks) {
  const auto conversions = std::make_shared<StringConversions>(std::move(hooks));
  const std::array<StringExportSpec, 22> specs{{
      {0x114u, "RtlAnsiStringToUnicodeString",
       [conversions](ExportCallContext& c) { return conversions->ansi_to_unicode(c); }},
      {0x115u, "RtlAppendStringToString", [](ExportCallContext& c) { return append_string(c, false); }},
      {0x116u, "RtlAppendUnicodeStringToString",
       [](ExportCallContext& c) { return append_string(c, true); }},
      {0x117u, "RtlAppendUnicodeToString", &rtl_append_unicode_to_string},
      {0x11Au, "RtlCompareMemory", &rtl_compare_memory},
      {0x11Cu, "RtlCompareString", &rtl_compare_string},
      {0x11Du, "RtlCompareStringN", &rtl_compare_string_n},
      {0x11Eu, "RtlCompareUnicodeString", &rtl_compare_unicode_string},
      {0x121u, "RtlCopyString", &rtl_copy_string},
      {0x122u, "RtlCopyUnicodeString", &rtl_copy_unicode_string},
      {0x123u, "RtlCreateUnicodeString",
       [conversions](ExportCallContext& c) { return conversions->create_unicode_string(c); }},
      {0x124u, "RtlDowncaseUnicodeChar", &rtl_downcase_unicode_char_export},
      {0x127u, "RtlFreeAnsiString",
       [conversions](ExportCallContext& c) { return conversions->free_string(c); }},
      {0x128u, "RtlFreeUnicodeString",
       [conversions](ExportCallContext& c) { return conversions->free_string(c); }},
      {0x132u, "RtlLowerChar", &rtl_lower_char},
      {0x133u, "RtlMultiByteToUnicodeN", &rtl_multi_byte_to_unicode_n},
      {0x134u, "RtlMultiByteToUnicodeSize", &rtl_multi_byte_to_unicode_size},
      {0x142u, "RtlUnicodeStringToAnsiString",
       [conversions](ExportCallContext& c) { return conversions->unicode_to_ansi(c); }},
      {0x143u, "RtlUnicodeToMultiByteN", &rtl_unicode_to_multi_byte_n},
      {0x144u, "RtlUnicodeToMultiByteSize", &rtl_unicode_to_multi_byte_size},
      {0x149u, "RtlUpcaseUnicodeChar", &rtl_upcase_unicode_char_export},
      {0x14Au, "RtlUpperChar", &rtl_upper_char},
  }};
  for (const auto& spec : specs) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = spec.name;
    descriptor.ordinal = spec.ordinal;
    descriptor.requirement = core::ExportRequirement::Required;
    descriptor.handler = spec.handler;
    if (!registry.register_export(std::move(descriptor))) return false;
  }
  return true;
}

}  // namespace xenon::xbox
