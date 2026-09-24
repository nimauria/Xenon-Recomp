#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace xenon::xbox {

// Canonical Xbox 360 system export metadata.
// This is a knowledge base of known Xbox 360 kernel/system exports
// independent of implementation. An export may be listed here without
// being implemented; the registry's handler is what determines if the
// export is actually callable.
//
// This structure is used for:
// 1. Diagnostic naming (unresolved "xboxkrnl!RtlImageXexHeaderField" vs bare ordinal)
// 2. Categorization (function vs variable, when known)
// 3. Documentation of the API surface
//
// Implementation status is tracked separately in ExportRegistry.

enum class ExportKind : std::uint8_t {
  Unknown = 0,
  Function,    // Callable function
  Variable,    // Guest-backed variable export
};

struct ExportMetadata {
  std::string_view module;
  std::uint16_t ordinal;
  std::string_view canonical_name;
  ExportKind kind;
};

// Look up metadata for an Xbox 360 system export.
// Returns nullptr if the ordinal is unknown in the metadata registry.
const ExportMetadata* lookup_export_metadata(std::string_view module,
                                            std::uint16_t ordinal);

// Look up metadata by name.
// Returns nullptr if not found.
const ExportMetadata* lookup_export_metadata_by_name(std::string_view module,
                                                     std::string_view name);

}  // namespace xenon::xbox
