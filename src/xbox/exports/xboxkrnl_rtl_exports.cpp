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
