#include "xenon/xbox/export_metadata.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace xenon::xbox {
namespace {

// Xbox 360 xboxkrnl RTL exports: a representative sampling of functions
// present in real titles' startup path. This is not an exhaustive export
// table; add entries as needed for compatibility work.
// 
// Names/ordinals sourced from:
// 1. Xenia's export table (referenced in task "Research rule")
// 2. Xbox 360 ABI documentation
// 3. ReXGlue/xboxkrnl ordinal analysis
//
// NOTE: "Known here" does NOT mean "implemented here". An export in this
// table may still be unimplemented; the ExportRegistry determines what
// is actually callable.

constexpr ExportMetadata kXboxkrnlRtlExports[] = {
    {"xboxkrnl.exe", 0x0129u, "RtlInitAnsiString", ExportKind::Function},
    {"xboxkrnl.exe", 0x012Au, "RtlInitUnicodeString", ExportKind::Function},
    {"xboxkrnl.exe", 0x012Bu, "RtlImageXexHeaderField", ExportKind::Function},
    {"xboxkrnl.exe", 0x012Cu, "RtlInitializeCriticalSection", ExportKind::Function},
    {"xboxkrnl.exe", 0x012Du, "RtlLeaveCriticalSection", ExportKind::Function},
    {"xboxkrnl.exe", 0x012Eu, "RtlEnterCriticalSection", ExportKind::Function},
    {"xboxkrnl.exe", 0x0136u, "RtlAnsiStringToUnicodeString", ExportKind::Function},
    {"xboxkrnl.exe", 0x0148u, "RtlCompareMemory", ExportKind::Function},
    {"xboxkrnl.exe", 0x014Bu, "RtlFillMemory", ExportKind::Function},
    {"xboxkrnl.exe", 0x014Cu, "RtlZeroMemory", ExportKind::Function},
};

// Normalize module name for lookup (remove .exe/.xex suffix, lowercase)
std::string normalize_module_name(std::string_view module) {
  std::string result(module);
  // Lowercase
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  // Strip .exe/.xex
  if (result.ends_with(".exe")) {
    result.resize(result.size() - 4);
  } else if (result.ends_with(".xex")) {
    result.resize(result.size() - 4);
  }
  return result;
}

bool module_matches(std::string_view metadata_module, std::string_view query_module) {
  const auto normalized = normalize_module_name(query_module);
  return metadata_module == normalized;
}

}  // namespace

const ExportMetadata* lookup_export_metadata(std::string_view module,
                                             std::uint16_t ordinal) {
  for (const auto& entry : kXboxkrnlRtlExports) {
    if (module_matches(entry.module, module) && entry.ordinal == ordinal) {
      return &entry;
    }
  }
  return nullptr;
}

const ExportMetadata* lookup_export_metadata_by_name(std::string_view module,
                                                     std::string_view name) {
  for (const auto& entry : kXboxkrnlRtlExports) {
    if (module_matches(entry.module, module) && entry.canonical_name == name) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace xenon::xbox
