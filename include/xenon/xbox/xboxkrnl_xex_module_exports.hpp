#pragma once

namespace xenon::core {
struct ExportCallContext;
class ExportRegistry;
}
namespace xenon::xbox {
struct XexImage;
class GuestModuleRegistry;
}

namespace xenon::xbox {

// XexCheckExecutablePrivilege (ordinal 0x194 / 404) - verified against the
// xenia-project/xenia xboxkrnl export table (xboxkrnl_table.inc) and its real
// implementation (xboxkrnl_modules.cc): tests a single bit of the current
// title's XEX_HEADER_SYSTEM_FLAGS optional header (key 0x00030000, an inline
// 32-bit value - see docs/recomp's optional-header size_class notes), where
// `privilege` is a BIT POSITION (not a mask): mask = 1 << privilege. Returns
// TRUE (guest r3 = 1) iff that bit is set in the XEX's system flags, FALSE
// (0) if the bit is clear OR the XEX has no such optional header at all.
//
// Reads only the host-side XexImage::header_bytes already parsed at load
// time - this export takes no guest pointer argument (unlike
// RtlImageXexHeaderField), so there is no guest-memory indirection to
// perform.
[[nodiscard]] bool xex_check_executable_privilege_export(const XexImage& image,
                                                          core::ExportCallContext& context);

// XexGetModuleHandle (0x195) and XexGetProcedureAddress (0x197): see the
// definitions for the guest ABI. Both are backed by GuestModuleRegistry, which
// owns the guest-visible module records and the dynamic export thunks.
[[nodiscard]] bool xex_get_module_handle_export(GuestModuleRegistry& registry,
                                                core::ExportCallContext& context);
[[nodiscard]] bool xex_get_procedure_address_export(GuestModuleRegistry& registry,
                                                    core::ExportCallContext& context);

// Convenience registrar for callers (tests/tools) that already have a
// constructed XexImage. XenonSession::init_exports() instead registers a
// lambda that lazily dereferences loaded_xex_ at call time (the XEX is not
// loaded yet when exports are registered) - see xboxkrnl_sync_exports.hpp's
// identical note for why.
[[nodiscard]] bool register_xboxkrnl_xex_module_exports(core::ExportRegistry& registry,
                                                         const XexImage& image);

}  // namespace xenon::xbox
