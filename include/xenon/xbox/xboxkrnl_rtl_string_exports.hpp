#pragma once

#include <cstdint>
#include <functional>

namespace xenon::core {
class ExportRegistry;
}

namespace xenon::xbox {

// Hooks the string exports use to reach the kernel pool. Conversions that
// allocate their destination buffer (RtlUnicodeStringToAnsiString with
// AllocateDestinationString, RtlCreateUnicodeString, ...) and the Free*String
// exports that release such a buffer go through these; XenonSession wires them
// to the process's KernelPool lazily (the process does not exist yet when
// exports are registered). An empty hook set is valid: an allocating conversion
// then fails with STATUS_NO_MEMORY and a free only clears the string header, which
// is the complete action for a string that aliases caller-owned memory.
struct RtlPoolHooks {
  // Returns a guest address for `size` bytes, or 0.
  std::function<std::uint32_t(std::uint32_t size)> allocate;
  // Releases a pool allocation; false if the address is not one (an aliased
  // buffer, which is left alone).
  std::function<bool(std::uint32_t address)> free;
};

// The xboxkrnl Rtl ANSI_STRING/UNICODE_STRING and character-conversion family:
// RtlCompareMemory, RtlCompareString(N), RtlCompareUnicodeString, RtlCopyString,
// RtlCopyUnicodeString, RtlAppend*ToString, RtlLowerChar/UpperChar,
// RtlUpcaseUnicodeChar/RtlDowncaseUnicodeChar, RtlMultiByteToUnicodeN/Size,
// RtlUnicodeToMultiByteN/Size, RtlAnsiStringToUnicodeString,
// RtlUnicodeStringToAnsiString, RtlCreateUnicodeString, RtlFreeAnsiString and
// RtlFreeUnicodeString. Native host code over guest memory; ordinals are from
// the xenia xboxkrnl export table. Safe to call repeatedly.
//
// Narrow<->wide conversion uses the Latin-1 ("ANSI") code page: each byte maps
// to the UTF-16 unit of the same value and any unit above 0xFF converts to '?'.
[[nodiscard]] bool register_xboxkrnl_rtl_string_exports(xenon::core::ExportRegistry& registry,
                                                        RtlPoolHooks hooks = {});

// Exposed for tests and other subsystems: the kernel's simple Unicode case maps.
[[nodiscard]] std::uint16_t rtl_upcase_unicode_char(std::uint16_t c) noexcept;
[[nodiscard]] std::uint16_t rtl_downcase_unicode_char(std::uint16_t c) noexcept;

}  // namespace xenon::xbox
