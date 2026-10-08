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
// Names/ordinals cross-checked against Xenia's xboxkrnl export table:
// https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc
//
// NOTE: "Known here" does NOT mean "implemented here". An export in this
// table may still be unimplemented; the ExportRegistry determines what
// is actually callable.

constexpr ExportMetadata kXboxkrnlRtlExports[] = {
    {"xboxkrnl.exe", 0x012Cu, "RtlInitAnsiString", ExportKind::Function},
    {"xboxkrnl.exe", 0x012Du, "RtlInitUnicodeString", ExportKind::Function},
    {"xboxkrnl.exe", 0x012Bu, "RtlImageXexHeaderField", ExportKind::Function},
    {"xboxkrnl.exe", 0x012Eu, "RtlInitializeCriticalSection", ExportKind::Function},
    {"xboxkrnl.exe", 0x0130u, "RtlLeaveCriticalSection", ExportKind::Function},
    {"xboxkrnl.exe", 0x0125u, "RtlEnterCriticalSection", ExportKind::Function},
    {"xboxkrnl.exe", 0x0114u, "RtlAnsiStringToUnicodeString", ExportKind::Function},
    {"xboxkrnl.exe", 0x011Au, "RtlCompareMemory", ExportKind::Function},
    {"xboxkrnl.exe", 0x011Bu, "RtlCompareMemoryUlong", ExportKind::Function},
    {"xboxkrnl.exe", 0x0126u, "RtlFillMemoryUlong", ExportKind::Function},
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
  return normalize_module_name(metadata_module) == normalize_module_name(query_module);
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
