#pragma once

#include <cstdint>

namespace xenon::xbox {

// Xbox 360 executable module record guest layout.
// Used by code that imports XexExecutableModuleHandle.
// This is NOT the host KernelModule, but a minimal guest-visible record
// used for XEX introspection (see RtlImageXexHeaderField, etc.).
//
// Known consumer: RtlImageXexHeaderField via module_record+0x58 -> XEX header.
// The loader maintains this record in stable guest memory and updates it
// when an effective image is loaded (base, or base+title-update).
//
// Current documented guest fields:
//   +0x58: pointer to guest XEX header (effective, including patches)
//
// Note: Xbox module/library records may contain other fields. Do not assume
// +0x58 is the only field. Treat unknown offsets as reserved/undocumented.
// When new fields are needed, add them via explicit named offsets rather
// than scattering raw constants.
struct XexModuleRecord {
  // Offset within guest memory where this record begins is opaque to this
  // layout; its consumer has the guest address. This struct describes offsets
  // relative to that base address.
  
  // +0x58: pointer to guest copy of XEX header (big-endian address).
  // This header is allocated and populated by the loader when an image is
  // mapped. On image reload (title update applied, module unloaded/reloaded),
  // the old header pointer becomes invalid; the new allocation and address
  // are written to this field.
  static constexpr std::uint32_t kXexHeaderPointerOffset = 0x58u;
};

}  // namespace xenon::xbox
