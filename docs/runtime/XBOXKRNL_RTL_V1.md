# Xboxkrnl RTL (Runtime Library) Service Layer

## Overview

The xboxkrnl RTL subsystem provides Xbox 360 runtime library exports needed by guest code during executable startup and normal operation. This includes XEX introspection, memory operations, string handling, and synchronization primitives.

## Architecture

### Export Registration

Xboxkrnl RTL exports are registered through the canonical `ExportRegistry` (see `xenon/core/export_registry.hpp`) during session initialization in `XenonSession::init_exports()`.

Entry point: `xenon::xbox::register_xboxkrnl_rtl_exports(ExportRegistry&)`

### Export Metadata

A separate metadata registry (`xenon/xbox/export_metadata.hpp`) maintains canonical knowledge of Xbox 360 system exports independent of implementation status. This supports:

- Diagnostic naming ("xboxkrnl!RtlImageXexHeaderField" vs bare ordinal)
- Classification (function vs variable, when known)
- Documentation of the API surface

An export may be listed in metadata without being implemented; the ExportRegistry handler determines if it is callable.

### Shared Optional-Header Logic

XEX optional-header interpretation is shared between:
1. The XEX loader (`src/xbox/xex/xex_loader.cpp`)
2. The RTL service layer (`src/xbox/rtl.cpp`)

Entry point: `xenon/xbox/rtl.hpp`

Functions:
- `find_xex_optional_header()`: locate an optional-header entry by key
- `resolve_xex_optional_header_field()`: apply Xbox semantics to return the field value

The XEX header layout includes a variable-size optional-header table where each entry is:
```
struct OptionalHeaderEntry {
  uint32_be key;
  uint32_be value_or_offset;
};
```

The low byte of the key selects the interpretation:
- `0x00`: inline value (use raw directly)
- `0x01`: inline offset (return address of value field within header)
- `0xFF`, `0x02-0xFE`: offset field (raw is offset from header base)

### Guest Module Record Layout

The guest module record backing `XexExecutableModuleHandle` resides in stable guest memory (allocated during `init_kernel_variable_exports()`). It is a minimal record used for XEX introspection:

```
// Offset within guest module record:
+0x58: pointer to guest copy of effective XEX header
```

The module record is zero-initialized. The XEX header pointer is populated when an image is loaded (see `refresh_dynamic_kernel_variables()`).

**Note**: The Xbox 360 module/library record format likely contains additional fields. Use named offsets and static assertions rather than raw constants scattered throughout the codebase. When new fields are required, extend this struct explicitly.

### Header Copy Ownership and Lifetime

When an XEX image is mapped:

1. `map_xex_image()` produces the effective image bytes (base or base+title-update)
2. `refresh_dynamic_kernel_variables()` allocates guest memory and copies the header
3. The guest header pointer is stored at module_record+0x58

**Lifetime contract**:
- The guest header allocation remains valid for the module's lifetime
- On image reload (title update reapplication, module reload), the old pointer becomes invalid and a new one is written
- `AddressSpace` reset owns cleanup of guest memory allocations
- Stale header pointers cannot occur if title-update paths are correct

## Implemented Exports

### RtlImageXexHeaderField (ordinal 0x12B / 299)

**Purpose**: Query a field from an XEX optional header.

**Guest ABI**:
```
r3 = XEX header guest pointer (guest-supplied, untrusted)
r4 = optional header key
-> r3 = result (field value, pointer, or 0 if not found)
```

**Implementation Notes**:
- Uses the guest-supplied header pointer directly, not the session's loaded_xex_
- Supports queries on arbitrary XEX images (user modules, DLLs, etc.)
- Treats malformed headers safely (returns 0 rather than crashing)
- Supports XEX1 and XEX2 consistently

**Validation**:
- Null header pointer returns 0
- Truncated/invalid headers return 0
- Excessive optional-header counts are bounded
- Offsets outside header bounds return 0

## Testing

Tests are located in `tests/xbox/rtl_tests.cpp` and use synthetic XEX headers (not proprietary game content):

1. **Inline value field** (class 0x00): returns inline value
2. **Inline offset field** (class 0x01): returns pointer to entry value
3. **Offset-backed field** (other classes): returns header_base + offset
4. **Absent key**: returns 0
5. **Null header**: returns 0 gracefully
6. **Truncated root header**: no host crash
7. **Excessive header count**: bounded, no out-of-bounds walk
8. **Out-of-range offset**: no invalid guest pointer returned
9. **Big-endian decoding**: correct byte order
10. **AC6 regression**: key=0x20401, class=0x01, dereferenced value matches expected heap size

## Known Limitations and Future Work

1. **Additional RTL exports**: The following are candidates for future implementation when needed:
   - RtlInitAnsiString
   - RtlInitUnicodeString
   - RtlInitializeCriticalSection / RtlEnterCriticalSection / RtlLeaveCriticalSection
   - RtlCompareMemory / RtlFillMemory / RtlZeroMemory
   
   These should be implemented only when:
   - Semantics are well understood
   - Xenon already has the underlying mechanism
   - Implementation can be correct and generic (no fake success)

2. **Critical section synchronization**: If implementing RTL critical-section exports, reuse Xenon's existing kernel synchronization primitives rather than building a separate host mutex table.

3. **Import diagnostics**: Unresolved imports now show canonical export names when known (see `export_metadata.cpp`), making compatibility gaps more transparent.

4. **XEX1 vs XEX2**: Both formats are supported for optional-header parsing. Detect format differences and implement real distinctions; do not silently reinterpret one format as the other.

## Build Configuration

The RTL subsystem is compiled as part of `xenon_xbox_kernel_io` (when both `XENON_ENABLE_KERNEL` and `XENON_ENABLE_MEMORY` are enabled).

Source files:
- `include/xenon/xbox/rtl.hpp`
- `src/xbox/rtl.cpp`
- `include/xenon/xbox/xex_module.hpp`
- `include/xenon/xbox/export_metadata.hpp`
- `src/xbox/export_metadata.cpp`
- `include/xenon/xbox/xboxkrnl_rtl_exports.hpp`
- `src/xbox/exports/xboxkrnl_rtl_exports.cpp`

Tests:
- `tests/xbox/rtl_tests.cpp`

## References

- [Xbox 360 Executable Format](docs/xbox/XEX_LOADER_V2.md)
- [Xboxkrnl Variable Exports](docs/runtime/RUNTIME_SESSION.md)
- [Runtime Session Architecture](docs/runtime/RUNTIME_SESSION.md)
