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

// Xbox 360 RTL export descriptors
struct RtlExportSpec {
  std::uint32_t ordinal;
  const char* name;
  core::ExportHandler handler;
};

// RtlImageXexHeaderField is the immediate blocker for AC6.
// Additional RTL functions can be implemented as needed.
const RtlExportSpec kRtlExports[] = {
    {0x012Bu, "RtlImageXexHeaderField", &rtl_image_xex_header_field},
    {0x0135u, "RtlNtStatusToDosError", &rtl_nt_status_to_dos_error_export},
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

    if (!registry.register_export(std::move(descriptor))) {
      return false;
    }
  }

  return true;
}

}  // namespace xenon::xbox
