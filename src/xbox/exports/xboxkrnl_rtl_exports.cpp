#include "xenon/core/export_registry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "xenon/xbox/export_metadata.hpp"
#include "xenon/xbox/rtl.hpp"

namespace xenon::xbox {
namespace {

using xenon::core::ExportCallContext;

// RtlImageXexHeaderField (ordinal 0x12B / 299)
// Resolves a field from an XEX optional header.
// 
// Guest ABI:
//   r3 = XEX header guest pointer (guest-supplied, untrusted)
//   r4 = optional header key (e.g., 0x20401 for DEFAULT_HEAP_SIZE)
//   -> r3 = result (field value, pointer, or 0 if not found)
//
// Return semantics:
//   - Key not found: r3 = 0
//   - Invalid header: r3 = 0
//   - Field resolved: r3 = inline value, pointer, or offset-relative address
//
// The implementation uses the guest-supplied header pointer directly, not
// the session's loaded_xex_, to support queries on arbitrary XEX images
// (user modules, DLLs, dynamically loaded executables, etc.).
bool rtl_image_xex_header_field(ExportCallContext& context) {
  const auto header_ptr =
      static_cast<xenon::cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto key = static_cast<std::uint32_t>(context.cpu.gpr[4]);

  // Treat the header pointer as untrusted; validate bounds and handle errors gracefully.
  std::string error_msg;
  const auto result =
      resolve_xex_optional_header_field(context.memory, header_ptr, key, &error_msg);

  // Return the result or 0 on error. Xbox convention for this API is 0 = not found.
  const auto return_value = result.value_or(0u);
  context.cpu.gpr[3] = static_cast<std::uint64_t>(return_value);

  return true;
}

// RtlNtStatusToDosError (ordinal 0x135 / 309)
// Converts an NTSTATUS to its Win32 error code equivalent, matching the real
// kernel's algorithm: success/customer-defined codes pass through unchanged,
// two status ranges self-encode their Win32 code in their low 16 bits, a
// table covers the well-known remaining STATUS_* codes, and anything still
// unmapped falls back to ERROR_MR_MID_NOT_FOUND - the same fallback the real
// kernel's own (much larger) table uses for gaps in its own coverage.
//
// Real AC6 repro: called (converting a STATUS_INVALID_HANDLE from a guest
// wait that raced an unready handle) with no case registered at all.
//
// Guest ABI: r3 = NTSTATUS -> r3 = Win32 error code.
std::uint32_t rtl_nt_status_to_dos_error(std::uint32_t status) {
  constexpr std::uint32_t kErrorMrMidNotFound = 317u;
  if (status == 0u || (status & 0x20000000u) != 0u) return status;
  if ((status >> 16) == 0x8007u) return status & 0xFFFFu;

  switch (status) {
    case 0x00000102u: return 258u;    // STATUS_TIMEOUT -> WAIT_TIMEOUT
    case 0x00000103u: return 997u;    // STATUS_PENDING -> ERROR_IO_PENDING
    case 0xC0000001u: return 31u;     // STATUS_UNSUCCESSFUL -> ERROR_GEN_FAILURE
    case 0xC0000002u: return 120u;    // STATUS_NOT_IMPLEMENTED -> ERROR_CALL_NOT_IMPLEMENTED
    case 0xC0000005u: return 998u;    // STATUS_ACCESS_VIOLATION -> ERROR_NOACCESS
    case 0xC0000008u: return 6u;      // STATUS_INVALID_HANDLE -> ERROR_INVALID_HANDLE
    case 0xC000000Du: return 87u;     // STATUS_INVALID_PARAMETER -> ERROR_INVALID_PARAMETER
    case 0xC0000010u: return 1u;      // STATUS_INVALID_DEVICE_REQUEST -> ERROR_INVALID_FUNCTION
    case 0xC0000011u: return 38u;     // STATUS_END_OF_FILE -> ERROR_HANDLE_EOF
    case 0xC0000017u: return 8u;      // STATUS_NO_MEMORY -> ERROR_NOT_ENOUGH_MEMORY
    case 0xC0000022u: return 5u;      // STATUS_ACCESS_DENIED -> ERROR_ACCESS_DENIED
    case 0xC0000023u: return 122u;    // STATUS_BUFFER_TOO_SMALL -> ERROR_INSUFFICIENT_BUFFER
    case 0xC0000024u: return 6u;      // STATUS_OBJECT_TYPE_MISMATCH -> ERROR_INVALID_HANDLE
    case 0xC0000034u: return 2u;      // STATUS_OBJECT_NAME_NOT_FOUND -> ERROR_FILE_NOT_FOUND
    case 0xC0000035u: return 183u;    // STATUS_OBJECT_NAME_COLLISION -> ERROR_ALREADY_EXISTS
    case 0xC000003Au: return 3u;      // STATUS_OBJECT_PATH_NOT_FOUND -> ERROR_PATH_NOT_FOUND
    case 0xC0000043u: return 32u;     // STATUS_SHARING_VIOLATION -> ERROR_SHARING_VIOLATION
    case 0xC000007Fu: return 112u;    // STATUS_DISK_FULL -> ERROR_DISK_FULL
    case 0xC00000A3u: return 21u;     // STATUS_DEVICE_NOT_READY -> ERROR_NOT_READY
    case 0xC00000BBu: return 50u;     // STATUS_NOT_SUPPORTED -> ERROR_NOT_SUPPORTED
    case 0xC0000120u: return 995u;    // STATUS_CANCELLED -> ERROR_OPERATION_ABORTED
    case 0xC000009Au: return 1450u;   // STATUS_INSUFFICIENT_RESOURCES -> ERROR_NO_SYSTEM_RESOURCES
    case 0xC0000225u: return 1168u;   // STATUS_NOT_FOUND -> ERROR_NOT_FOUND
    default: break;
  }

  if ((status >> 16) == 0xC001u) return status & 0xFFFFu;
  return kErrorMrMidNotFound;
}

bool rtl_nt_status_to_dos_error_export(ExportCallContext& context) {
  const auto source_status = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  context.cpu.gpr[3] = rtl_nt_status_to_dos_error(source_status);
  return true;
}

// RtlCompareMemoryUlong (ordinal 0x11B / 283)
// Guest ABI: r3 = Source1, r4 = Source2, r5 = Length (bytes) -> r3 = number
// of leading bytes that matched (real semantics: compares as an array of
// 4-byte ULONGs, so only whole 4-byte units - any trailing 1-3 bytes past
// the last full ULONG are never compared and never counted, matching real
// hardware exactly, not a Xenon shortcut).
bool rtl_compare_memory_ulong_export(ExportCallContext& context) {
  const auto source1 = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto source2 = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto length = static_cast<std::uint32_t>(context.cpu.gpr[5]);

  std::uint32_t matched = 0;
  const std::uint32_t whole_ulongs = length & ~3u;
  for (; matched < whole_ulongs; matched += 4u) {
    if (context.memory.read32_be(source1 + matched) !=
        context.memory.read32_be(source2 + matched)) {
      break;
    }
  }
  context.cpu.gpr[3] = matched;
  return true;
}

// RtlFillMemoryUlong (ordinal 0x126 / 294)
// Guest ABI: r3 = Destination, r4 = Length (bytes), r5 = Pattern (ULONG) ->
// void. Fills whole 4-byte units only (any trailing 1-3 bytes are left
// untouched, matching real hardware).
bool rtl_fill_memory_ulong_export(ExportCallContext& context) {
  const auto destination = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto length = static_cast<std::uint32_t>(context.cpu.gpr[4]);
  const auto pattern = static_cast<std::uint32_t>(context.cpu.gpr[5]);

  const std::uint32_t whole_ulongs = length & ~3u;
  for (std::uint32_t offset = 0; offset < whole_ulongs; offset += 4u) {
    context.memory.write32_be(destination + offset, pattern);
  }
  return true;
}

// RtlInitAnsiString (ordinal 0x12C / 300)
// Guest ABI: r3 = PSTRING (ANSI_STRING) DestinationString, r4 = PCSTR
// SourceString (nullable) -> void. Points the destination struct directly
// at the caller's existing buffer - real hardware does not copy or allocate
// here, so DestinationString->Buffer aliases SourceString exactly.
bool rtl_init_ansi_string_export(ExportCallContext& context) {
  const auto dest = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto source = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  if (dest == 0u) return true;

  if (source == 0u) {
    context.memory.write16_be(dest + 0u, 0u);      // Length
    context.memory.write16_be(dest + 2u, 0u);      // MaximumLength
    context.memory.write32_be(dest + 4u, 0u);      // Buffer
    return true;
  }

  std::uint32_t length = 0;
  while (length < 0xFFFEu && context.memory.read8(source + length) != 0u) {
    ++length;
  }
  context.memory.write16_be(dest + 0u, static_cast<std::uint16_t>(length));
  context.memory.write16_be(dest + 2u, static_cast<std::uint16_t>(length + 1u));
  context.memory.write32_be(dest + 4u, source);
  return true;
}

// RtlFreeAnsiString (ordinal 0x127) now lives with the rest of the ANSI_STRING/
// UNICODE_STRING family in xboxkrnl_rtl_string_exports.cpp, where it can release
// a pool-allocated buffer (see RtlPoolHooks).

// RtlInitUnicodeString (ordinal 0x12D / 301)
// Guest ABI: r3 = PUNICODE_STRING DestinationString, r4 = PCWSTR
// SourceString (nullable, UTF-16BE guest string) -> void. Same aliasing
// contract as RtlInitAnsiString: Buffer points directly at SourceString.
bool rtl_init_unicode_string_export(ExportCallContext& context) {
  const auto dest = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto source = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  if (dest == 0u) return true;

  if (source == 0u) {
    context.memory.write16_be(dest + 0u, 0u);
    context.memory.write16_be(dest + 2u, 0u);
    context.memory.write32_be(dest + 4u, 0u);
    return true;
  }

  std::uint32_t char_count = 0;
  while (char_count < 0x7FFEu &&
         context.memory.read16_be(source + char_count * 2u) != 0u) {
    ++char_count;
  }
  const auto length_bytes = static_cast<std::uint16_t>(char_count * 2u);
  context.memory.write16_be(dest + 0u, length_bytes);
  context.memory.write16_be(dest + 2u, static_cast<std::uint16_t>(length_bytes + 2u));
  context.memory.write32_be(dest + 4u, source);
  return true;
}

// civil_from_days/days_from_civil: Howard Hinnant's well-known constexpr
// Gregorian calendar <-> day-count algorithms (public domain, from
// http://howardhinnant.github.io/date_algorithms.html), valid for the
// entire proleptic Gregorian calendar - exact by construction, not an
// approximation, so there is no meaningful "risk" in this arithmetic beyond
// transcribing it correctly (verified against known reference dates in this
// file's tests).
struct CivilDate {
  std::int64_t year;
  std::uint32_t month;  // 1-12
  std::uint32_t day;    // 1-31
};

CivilDate civil_from_days(std::int64_t z) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<std::uint64_t>(z - era * 146097);              // [0, 146096]
  const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;     // [0, 399]
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                  // [0, 365]
  const auto mp = (5 * doy + 2) / 153;                                       // [0, 11]
  const auto d = static_cast<std::uint32_t>(doy - (153 * mp + 2) / 5 + 1);   // [1, 31]
  const auto m = static_cast<std::uint32_t>(mp < 10 ? mp + 3 : mp - 9);      // [1, 12]
  return CivilDate{y + (m <= 2 ? 1 : 0), m, d};
}

std::int64_t days_from_civil(std::int64_t y, std::uint32_t m, std::uint32_t d) {
  y -= (m <= 2);
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<std::uint64_t>(y - era * 400);                        // [0, 399]
  const auto doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;                    // [0, 365]
  const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                             // [0, 146096]
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Weekday for a day-count z where z=0 (1970-01-01) is Thursday (weekday 4),
// mapped to the real TIME_FIELDS convention (0 = Sunday).
std::uint32_t weekday_from_days(std::int64_t z) {
  return static_cast<std::uint32_t>(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

// 100ns ticks between the FILETIME/Xbox LARGE_INTEGER epoch (1601-01-01) and
// the Unix epoch (1970-01-01) - the standard, widely-published constant.
constexpr std::int64_t kFiletimeToUnixEpoch100ns = 116444736000000000LL;
constexpr std::int64_t kTicksPerSecond = 10'000'000LL;
constexpr std::int64_t kSecondsPerDay = 86400LL;

// RtlTimeToTimeFields (ordinal 0x140 / 320)
// Guest ABI: r3 = PLARGE_INTEGER Time (100ns ticks since 1601-01-01, guest
// int64), r4 = PTIME_FIELDS TimeFields (out struct: 8x CSHORT - Year, Month,
// Day, Hour, Minute, Second, Milliseconds, Weekday) -> void. A pure UTC
// calendar conversion; real hardware applies no timezone/DST here.
bool rtl_time_to_time_fields_export(ExportCallContext& context) {
  const auto time_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto fields_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  if (time_ptr == 0u || fields_ptr == 0u) return true;

  const auto raw_100ns = static_cast<std::int64_t>(context.memory.read64_be(time_ptr));
  const std::int64_t unix_100ns = raw_100ns - kFiletimeToUnixEpoch100ns;

  std::int64_t total_seconds = unix_100ns / kTicksPerSecond;
  std::int64_t remainder_100ns = unix_100ns % kTicksPerSecond;
  if (remainder_100ns < 0) {
    remainder_100ns += kTicksPerSecond;
    total_seconds -= 1;
  }
  std::int64_t days = total_seconds / kSecondsPerDay;
  std::int64_t seconds_in_day = total_seconds % kSecondsPerDay;
  if (seconds_in_day < 0) {
    seconds_in_day += kSecondsPerDay;
    days -= 1;
  }

  const auto civil = civil_from_days(days);
  const auto hour = static_cast<std::uint32_t>(seconds_in_day / 3600);
  const auto minute = static_cast<std::uint32_t>((seconds_in_day % 3600) / 60);
  const auto second = static_cast<std::uint32_t>(seconds_in_day % 60);
  const auto milliseconds = static_cast<std::uint32_t>(remainder_100ns / 10'000);
  const auto weekday = weekday_from_days(days);

  context.memory.write16_be(fields_ptr + 0u, static_cast<std::uint16_t>(civil.year));
  context.memory.write16_be(fields_ptr + 2u, static_cast<std::uint16_t>(civil.month));
  context.memory.write16_be(fields_ptr + 4u, static_cast<std::uint16_t>(civil.day));
  context.memory.write16_be(fields_ptr + 6u, static_cast<std::uint16_t>(hour));
  context.memory.write16_be(fields_ptr + 8u, static_cast<std::uint16_t>(minute));
  context.memory.write16_be(fields_ptr + 10u, static_cast<std::uint16_t>(second));
  context.memory.write16_be(fields_ptr + 12u, static_cast<std::uint16_t>(milliseconds));
  context.memory.write16_be(fields_ptr + 14u, static_cast<std::uint16_t>(weekday));
  return true;
}

// RtlTimeFieldsToTime (ordinal 0x13F / 319)
// Guest ABI: r3 = PTIME_FIELDS TimeFields (in struct, Weekday ignored - not
// part of the round trip on real hardware either), r4 = PLARGE_INTEGER Time
// (out, 100ns ticks since 1601-01-01) -> r3 = BOOLEAN (TRUE/1 if the fields
// described a valid date/time, FALSE/0 otherwise - matching real semantics;
// callers use this to detect a corrupt/out-of-range TIME_FIELDS).
bool rtl_time_fields_to_time_export(ExportCallContext& context) {
  const auto fields_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto time_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  if (fields_ptr == 0u || time_ptr == 0u) {
    context.cpu.gpr[3] = 0u;
    return true;
  }

  const auto year = static_cast<std::int64_t>(context.memory.read16_be(fields_ptr + 0u));
  const auto month = static_cast<std::uint32_t>(context.memory.read16_be(fields_ptr + 2u));
  const auto day = static_cast<std::uint32_t>(context.memory.read16_be(fields_ptr + 4u));
  const auto hour = static_cast<std::uint32_t>(context.memory.read16_be(fields_ptr + 6u));
  const auto minute = static_cast<std::uint32_t>(context.memory.read16_be(fields_ptr + 8u));
  const auto second = static_cast<std::uint32_t>(context.memory.read16_be(fields_ptr + 10u));
  const auto milliseconds =
      static_cast<std::uint32_t>(context.memory.read16_be(fields_ptr + 12u));

  if (month < 1u || month > 12u || day < 1u || day > 31u || hour > 23u || minute > 59u ||
      second > 59u || milliseconds > 999u) {
    context.cpu.gpr[3] = 0u;
    return true;
  }

  const auto days = days_from_civil(year, month, day);
  const std::int64_t total_seconds =
      days * kSecondsPerDay + hour * 3600 + minute * 60 + second;
  const std::int64_t unix_100ns = total_seconds * kTicksPerSecond + milliseconds * 10'000;
  const std::int64_t raw_100ns = unix_100ns + kFiletimeToUnixEpoch100ns;

  context.memory.write64_be(time_ptr, static_cast<std::uint64_t>(raw_100ns));
  context.cpu.gpr[3] = 1u;
  return true;
}

// RtlCaptureContext (ordinal 0x119)
// Guest ABI: r3 = guest CONTEXT* to fill -> void (real hardware never
// returns a status here).
//
// Deliberately does not touch guest memory at all: the real Xbox 360/Win32
// CONTEXT record's guest-visible field layout could not be verified against
// any available reference (xenia never implements this function's body
// either - only declares the ordinal - and hedge-dev/UnleashedRecomp, a
// real shipped Xbox 360 static-recompilation PC port, stubs it identically:
// log and return with no memory writes at all). Writing a guessed-size or
// guessed-layout buffer would risk corrupting whatever real guest data
// follows it on the stack, or feeding a real consumer plausible-looking
// garbage instead of the honest "we did not fill this" signal a no-op
// preserves. RtlCaptureContext only ever feeds RtlUnwind/exception dispatch
// (also not implemented - see RtlRaiseException/RtlUnwind), so there is no
// currently-reachable consumer of this buffer's contents to get right or
// wrong yet.
bool rtl_capture_context_export(ExportCallContext&) { return true; }

// __C_specific_handler (ordinal 0x1A5)
// Guest ABI: MSVC SEH personality-routine convention (exception record,
// establisher frame, context, dispatcher context) -> exception disposition.
//
// Only ever invoked BY a real unwind engine walking a function's exception
// scope table during dispatch - never called directly by ordinary guest
// code. Xenon has no such engine (see RtlUnwind), so this is currently
// unreachable dead code in practice; implemented as the honest, spec-correct
// response for a personality routine asked to evaluate a scope table it
// cannot process: report ExceptionContinueSearch (1) rather than fabricating
// a handled/continue-execution disposition it did not earn. Matches hedge-
// dev/UnleashedRecomp's identical no-op-and-return-cleanly precedent for
// this function in a real, shipped Xbox 360 static-recomp title.
bool c_specific_handler_export(ExportCallContext& context) {
  constexpr std::uint32_t kExceptionContinueSearch = 1u;
  context.cpu.gpr[3] = kExceptionContinueSearch;
  return true;
}

// Xbox 360 RTL export descriptors
struct RtlExportSpec {
  std::uint32_t ordinal;
  const char* name;
  core::ExportHandler handler;
  bool partial{false};
  const char* partial_note{};
};

// RtlImageXexHeaderField is the immediate blocker for AC6.
// Additional RTL functions can be implemented as needed.
const RtlExportSpec kRtlExports[] = {
    {0x0119u, "RtlCaptureContext", &rtl_capture_context_export, true,
     "does not fill the guest CONTEXT record - real Xbox 360 field layout "
     "could not be verified against any available reference, and this "
     "buffer currently has no reachable real consumer (RtlUnwind/exception "
     "dispatch are not implemented either); matches xenia (never implements "
     "this function's body) and hedge-dev/UnleashedRecomp (identical no-op "
     "stub in a real shipped title)"},
    {0x01A5u, "__C_specific_handler", &c_specific_handler_export, true,
     "always reports ExceptionContinueSearch - only reachable from a real "
     "unwind engine Xenon does not implement (RtlUnwind is not implemented "
     "either), so this is currently unreachable dead code; matches hedge-"
     "dev/UnleashedRecomp's identical no-op precedent in a real shipped "
     "title"},
    {0x012Bu, "RtlImageXexHeaderField", &rtl_image_xex_header_field},
    {0x0135u, "RtlNtStatusToDosError", &rtl_nt_status_to_dos_error_export},
    {0x011Bu, "RtlCompareMemoryUlong", &rtl_compare_memory_ulong_export},
    {0x0126u, "RtlFillMemoryUlong", &rtl_fill_memory_ulong_export},
    {0x012Cu, "RtlInitAnsiString", &rtl_init_ansi_string_export},
    {0x012Du, "RtlInitUnicodeString", &rtl_init_unicode_string_export},
    {0x013Fu, "RtlTimeFieldsToTime", &rtl_time_fields_to_time_export},
    {0x0140u, "RtlTimeToTimeFields", &rtl_time_to_time_fields_export},
};

}  // namespace

bool register_xboxkrnl_rtl_exports(core::ExportRegistry& registry) {
  // Convert RtlExportSpec entries to ExportDescriptor format
  for (const auto& spec : kRtlExports) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = spec.name;
    descriptor.ordinal = spec.ordinal;
    descriptor.handler = spec.handler;
    descriptor.requirement = core::ExportRequirement::Required;
    descriptor.partial = spec.partial;
    if (spec.partial_note) descriptor.partial_note = spec.partial_note;

    if (!registry.register_export(std::move(descriptor))) {
      return false;
    }
  }

  return true;
}

}  // namespace xenon::xbox
